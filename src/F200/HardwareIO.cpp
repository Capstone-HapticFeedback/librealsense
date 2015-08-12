#include "../Common.h"

#include "F200Types.h"
#include "Calibration.h"
#include "Projection.h"
#include "HardwareIO.h"

#include <libuvc/libuvc.h>

#include <thread>
#include <atomic>
#include <mutex>

using namespace f200;

#ifndef WIN32

#define IVCAM_VID                       0x8086
#define IVCAM_PID                       0x0A66
#define IVCAM_MONITOR_INTERFACE         0x4
#define IVCAM_MONITOR_ENDPOINT_OUT      0x1
#define IVCAM_MONITOR_ENDPOINT_IN       0x81
#define IVCAM_MONITOR_MAGIC_NUMBER      0xcdab
#define IVCAM_MIN_SUPPORTED_VERSION     13
#define IVCAM_MONITOR_MAX_BUFFER_SIZE   1024
#define IVCAM_MONITOR_HEADER_SIZE       (sizeof(uint32_t)*6)
#define IVCAM_MONITOR_MUTEX_TIMEOUT     3000

#define NUM_OF_CALIBRATION_PARAMS       (100)
#define HW_MONITOR_COMMAND_SIZE         (1000)
#define HW_MONITOR_BUFFER_SIZE          (1000)
#define PARAMETERS_BUFFER_SIZE          (50)

#define MAX_SIZE_OF_CALIB_PARAM_BYTES   (800)
#define SIZE_OF_CALIB_PARAM_BYTES       (512)
#define SIZE_OF_CALIB_HEADER_BYTES      (4)

enum IVCAMMonitorCommand
{
    UpdateCalib         = 0xBC,
    GetIRTemp           = 0x52,
    GetMEMSTemp         = 0x0A,
    HWReset             = 0x28,
    GVD                 = 0x3B,
    BIST                = 0xFF,
    GoToDFU             = 0x80,
    GetCalibrationTable = 0x3D,
    DebugFormat         = 0x0B,
    TimeStempEnable     = 0x0C,
    GetPowerGearState   = 0xFF,
    SetDefaultControls  = 0xA6,
    GetDefaultControls  = 0xA7,
    GetFWLastError      = 0x0E,
    CheckI2cConnect     = 0x4A,
    CheckRGBConnect     = 0x4B,
    CheckDPTConnect     = 0x4C
};

int bcdtoint(uint8_t * buf, int bufsize)
{
    int r = 0;
    for(int i = 0; i < bufsize; i++)
        r = r * 10 + *buf++;
    return r;
}

int getVersionOfCalibration(uint8_t * validation, uint8_t * version)
{
    uint8_t valid[2] = {0X14, 0x0A};
    if (memcmp(valid, validation, 2) != 0) return 0;
    else return bcdtoint(version, 2);
}

//////////////////////////
// Private Hardware I/O //
//////////////////////////

class f200::IVCAMHardwareIOInternal
{
    libusb_device_handle * usbDeviceHandle = nullptr;
    std::timed_mutex usbMutex;
    
    CameraCalibrationParameters parameters;
    
    std::thread temperatureThread;
    std::atomic<bool> isTemperatureThreadRunning;
    
    int PrepareUSBCommand(uint8_t * request, size_t & requestSize, uint32_t op,
                          uint32_t p1 = 0, uint32_t p2 = 0, uint32_t p3 = 0, uint32_t p4 = 0,
                          uint8_t * data = 0, size_t dataLength = 0)
    {
        
        if (requestSize < IVCAM_MONITOR_HEADER_SIZE)
            return 0;
        
        int index = sizeof(uint16_t);
        *(uint16_t *)(request + index) = IVCAM_MONITOR_MAGIC_NUMBER;
        index += sizeof(uint16_t);
        *(uint32_t *)(request + index) = op;
        index += sizeof(uint32_t);
        *(uint32_t *)(request + index) = p1;
        index += sizeof(uint32_t);
        *(uint32_t *)(request + index) = p2;
        index += sizeof(uint32_t);
        *(uint32_t *)(request + index) = p3;
        index += sizeof(uint32_t);
        *(uint32_t *)(request + index) = p4;
        index += sizeof(uint32_t);
        
        if (dataLength)
        {
            memcpy(request + index , data, dataLength);
            index += dataLength;
        }
        
        // Length doesn't include header size (sizeof(uint32_t))
        *(uint16_t *)request = (uint16_t)(index - sizeof(uint32_t));
        requestSize = index;
        return index;
    }
    
    int ExecuteUSBCommand(uint8_t *out, size_t outSize, uint32_t & op, uint8_t * in, size_t & inSize)
    {
        // write
        errno = 0;
        
        int outXfer;
        
        if (usbMutex.try_lock_for(std::chrono::milliseconds(IVCAM_MONITOR_MUTEX_TIMEOUT)))
        {
            int ret = libusb_bulk_transfer(usbDeviceHandle, IVCAM_MONITOR_ENDPOINT_OUT, out, (int) outSize, &outXfer, 1000); // timeout in ms
            
            if (ret < 0 )
            {
                printf("[libusb failure] libusb_bulk_transfer (endpoint_out) - status: %s", libusb_error_name(ret));
                return ret;
            }

            // Debugging only
            // dumpCommand(out, outSize);
            
            // read
            if (in && inSize)
            {
                uint8_t buf[IVCAM_MONITOR_MAX_BUFFER_SIZE];
                
                errno = 0;
                
                ret = libusb_bulk_transfer(usbDeviceHandle, IVCAM_MONITOR_ENDPOINT_IN, buf, sizeof(buf), &outXfer, 1000);
                
                if (outXfer < (int)sizeof(uint32_t))
                {
                    printf("[libusb failure] libusb_bulk_transfer (endpoint_in) - status: %s", libusb_error_name(ret));
                    usbMutex.unlock();
                    return -1;
                }
                else
                {
                    // Debuggong only
                    // dumpCommand(buf, outXfer);
                    // outXfer -= sizeof(uint32_t);
                    
                    op = *(uint32_t *)buf;
                    
                    if (outXfer > (int)inSize)
                    {
                        printf("usb_device_bulk_transfer IN failed: user buffer too small (%d:%zu)", outXfer, inSize);
                        usbMutex.unlock();
                        return -1;
                    }
                    else
                        inSize = outXfer;
                    memcpy(in, buf, inSize);
                }
            }
            
            usbMutex.unlock();
            return ret;
        }
        else
        {
            throw std::runtime_error("usbMutex timed out");
        }
    }
    
    void FillUSBBuffer(int opCodeNumber, int p1, int p2, int p3, int p4, char * data, int dataLength, char * bufferToSend, int & length)
    {
        uint16_t preHeaderData = IVCAM_MONITOR_MAGIC_NUMBER;
        
        char * writePtr = bufferToSend;
        int header_size = 4;
        
        // TBD !!! This may change. Need to define it as part of API
        //typeSize = sizeof(float);
        
        int cur_index = 2;
        *(uint16_t *)(writePtr + cur_index) = preHeaderData;
        cur_index += sizeof(uint16_t);
        *(int *)( writePtr + cur_index) = opCodeNumber;
        cur_index += sizeof(uint32_t);
        *(int *)( writePtr + cur_index) = p1;
        cur_index += sizeof(uint32_t);
        *(int *)( writePtr + cur_index) = p2;
        cur_index += sizeof(uint32_t);
        *(int *)( writePtr + cur_index) = p3;
        cur_index += sizeof(uint32_t);
        *(int *)( writePtr + cur_index) = p4;
        cur_index += sizeof(uint32_t);
        
        if (dataLength)
        {
            memcpy(writePtr + cur_index , data, dataLength);
            cur_index += dataLength;
        }
        
        length = cur_index;
        *(uint16_t *) bufferToSend = (uint16_t)(length - header_size);// Length doesn't include header
        
    }
    
    void GetCalibrationRawData(uint8_t * data, size_t & bytesReturned)
    {
        uint8_t request[IVCAM_MONITOR_HEADER_SIZE];
        size_t requestSize = sizeof(request);
        uint32_t responseOp;
        
        if ( !((PrepareUSBCommand(request, requestSize, GetCalibrationTable) > 0) && (ExecuteUSBCommand(request, requestSize, responseOp, data, bytesReturned) != -1)) )
            throw std::runtime_error("usb transfer to retrieve calibration data failed");
    }
    
    void ProjectionCalibrate(uint8_t * rawCalibData,int len, CameraCalibrationParameters * calprms)
    {
        uint8_t * bufParams = rawCalibData + 4;
        
        IVCAMCalibrator<float> * calibration = Projection::GetInstance()->GetCalibrationObject();
        
        CameraCalibrationParametersVersion CalibrationData;
        IVCAMTesterData TesterData;
        
        memset(&CalibrationData, 0, sizeof(CameraCalibrationParametersVersion));
        
        int ver = getVersionOfCalibration(bufParams, bufParams + 2);
        
        if (ver == IVCAM_MIN_SUPPORTED_VERSION)
        {
            float *params = (float *)bufParams;
            
            calibration->buildParameters(params, 100);
            
            // Debugging -- optional
            // calibration->PrintParameters();
            
            memcpy(calprms, params+1, sizeof(CameraCalibrationParameters));
            memcpy(&TesterData, bufParams, SIZE_OF_CALIB_HEADER_BYTES);
            
            memset((uint8_t*)&TesterData+SIZE_OF_CALIB_HEADER_BYTES,0,sizeof(IVCAMTesterData) - SIZE_OF_CALIB_HEADER_BYTES);
        }
        else if (ver > IVCAM_MIN_SUPPORTED_VERSION)
        {
            rawCalibData = rawCalibData + 4;
            
            int size = (sizeof(CameraCalibrationParametersVersion) > len) ? len : sizeof(CameraCalibrationParametersVersion);
            
            auto fixWithVersionInfo = [&](CameraCalibrationParametersVersion &d, int size, uint8_t * data)
            {
                memcpy((uint8_t*)&d + sizeof(int), data, size - sizeof(int));
            };
            
            fixWithVersionInfo(CalibrationData, size, rawCalibData);
            
            memcpy(calprms, &CalibrationData.CalibrationParameters, sizeof(CameraCalibrationParameters));
            calibration->buildParameters(CalibrationData.CalibrationParameters);
            
            // Debugging -- optional
            // calibration->PrintParameters();

            memcpy(&TesterData,  rawCalibData, SIZE_OF_CALIB_HEADER_BYTES);  //copy the header: valid + version
            
            //copy the tester data from end of calibration
            int EndOfCalibratioData = SIZE_OF_CALIB_PARAM_BYTES + SIZE_OF_CALIB_HEADER_BYTES;
            memcpy((uint8_t*)&TesterData + SIZE_OF_CALIB_HEADER_BYTES , rawCalibData + EndOfCalibratioData , sizeof(IVCAMTesterData) - SIZE_OF_CALIB_HEADER_BYTES);
            
            calibration->InitializeThermalData(TesterData.TemperatureData, TesterData.ThermalLoopParams);
        }
    }
    
    void ReadTemperatures(IVCAMTemperatureData & data)
    {
        data = {0};
        
        int IRTemp;
        if (!GetIRtemp(IRTemp))
            throw std::runtime_error("could not get IR temperature");
        
        data.IRTemp = (float) IRTemp;
        
        float LiguriaTemp;
        if (!GetMEMStemp(LiguriaTemp))
            throw std::runtime_error("could not get liguria temperature");
        
        data.LiguriaTemp = LiguriaTemp;
    }
    
    bool GetMEMStemp(float & MEMStemp)
    {
        /*
         TIVCAMCommandParameters CommandParameters;
         CommandParameters.CommandOp = HWmonitor_GetMEMSTemp;
         CommandParameters.Param1 = 0;
         CommandParameters.Param2 = 0;
         CommandParameters.Param3 = 0;
         CommandParameters.Param4 = 0;
         CommandParameters.sizeOfSendCommandData = 0;
         CommandParameters.TimeOut = 5000;
         CommandParameters.oneDirection = false;
         
         bool result = PerfomAndSendHWmonitorCommand(CommandParameters);
         if (result != true)
         return false;
         
         int32_t Temp = *((int32_t*)(CommandParameters.recivedCommandData));
         MEMStemp = (float) Temp ;
         MEMStemp /= 100;
         
         return true;
         */
        return false;
    }
    
    bool GetIRtemp(int & IRtemp)
    {
        /*
         TIVCAMCommandParameters CommandParameters;
         
         CommandParameters.CommandOp = HWmonitor_GetIRTemp;
         CommandParameters.Param1 = 0;
         CommandParameters.Param2 = 0;
         CommandParameters.Param3 = 0;
         CommandParameters.Param4 = 0;
         CommandParameters.sizeOfSendCommandData = 0;
         CommandParameters.TimeOut = 5000;
         CommandParameters.oneDirection = false;
         
         sts = PerfomAndSendHWmonitorCommand(CommandParameters);
         if (sts != IVCAM_SUCCESS)
         return IVCAM_FAILURE;
         
         IRtemp = (int8_t) CommandParameters.recivedCommandData[0];
         return IVCAM_SUCCESS;
         */
        return false;
    }
    
    /*
    void UpdateASICCoefs(TAsicCoefficiants * AsicCoefficiants)
    {

         TIVCAMCommandParameters CommandParameters;
         ETCalibTable FWres;
         
         TIVCAMStreamProfile IVCAMStreamProfile;
         FWres = ectVGA;
         
         CommandParameters.CommandOp = HWmonitor_UpdateCalib;
         memcpy(CommandParameters.data, AsicCoefficiants->CoefValueArray, NUM_OF_CALIBRATION_COEFFS*sizeof(float));
         CommandParameters.Param1 = FWres;
         CommandParameters.Param2 = 0;
         CommandParameters.Param3 = 0;
         CommandParameters.Param4 = 0;
         CommandParameters.oneDirection = false;
         CommandParameters.sizeOfSendCommandData = NUM_OF_CALIBRATION_COEFFS*sizeof(float);
         CommandParameters.TimeOut = 5000;
         
         return PerfomAndSendHWmonitorCommand(CommandParameters);
    }
    */
    
    void TemperatureControlLoop()
    {
        // @tofix
    }
    
public:
    
    IVCAMHardwareIOInternal(uvc_context_t * ctx)
    {
        if (!ctx) throw std::runtime_error("must pass libuvc context handle");
        
        libusb_context * usbctx = uvc_get_libusb_context(ctx);
        
        usbDeviceHandle = libusb_open_device_with_vid_pid(usbctx, IVCAM_VID, IVCAM_PID);
        
        if (usbDeviceHandle == NULL)
            throw std::runtime_error("libusb_open_device_with_vid_pid() failed");
        
        int status = libusb_claim_interface(usbDeviceHandle, IVCAM_MONITOR_INTERFACE);
        if (status < 0) throw std::runtime_error("libusb_claim_interface() failed");
        
        uint8_t rawCalibrationBuffer[HW_MONITOR_BUFFER_SIZE];
        size_t bufferLength = HW_MONITOR_BUFFER_SIZE;
        GetCalibrationRawData(rawCalibrationBuffer, bufferLength);
        
        CameraCalibrationParameters calibratedParameters;
        ProjectionCalibrate(rawCalibrationBuffer, (int) bufferLength, &calibratedParameters);
        
        parameters = calibratedParameters;
    }
    
    ~IVCAMHardwareIOInternal()
    {
        libusb_release_interface(usbDeviceHandle, IVCAM_MONITOR_INTERFACE);
    }
    
    bool StartTempCompensationLoop()
    {
        // @tofix
        return false;
    }
    
    void StopTempCompensationLoop()
    {
        // @tofix
    }
    
    CameraCalibrationParameters & GetParameters()
    {
        return parameters;
    }
    
};

/////////////////////////
// Public Hardware I/O //
/////////////////////////

IVCAMHardwareIO::IVCAMHardwareIO(uvc_context_t * ctx)
{
    internal.reset(new IVCAMHardwareIOInternal(ctx));
}

IVCAMHardwareIO::~IVCAMHardwareIO()
{
    
}

bool IVCAMHardwareIO::StartTempCompensationLoop()
{
    return internal->StartTempCompensationLoop();
}

void IVCAMHardwareIO::StopTempCompensationLoop()
{
    internal->StopTempCompensationLoop();
}

CameraCalibrationParameters & IVCAMHardwareIO::GetParameters()
{
    return internal->GetParameters();
}

#endif