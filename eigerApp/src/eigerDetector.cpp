/* eigerDetector.cpp
 *
 * This is a driver for a Eiger pixel array detector.
 *
 * Authors: Bruno Martins
 *          Diego Omitto
 *          Brookhaven National Laboratory
 *
 * Created: March 30, 2015
 *
 */

#include <epicsExport.h>
#include <epicsThread.h>
#include <iocsh.h>
#include <asynOctetSyncIO.h>
#include <math.h>

#include <vector>
#include <hdf5.h>
#include <hdf5_hl.h>
#include <iostream>

#include <frozen.h> // JSON parser

#include "ADDriver.h"

/** Messages to/from server */
#define GET_FILE_RETRIES    10
#define MAX_MESSAGE_SIZE    65536
#define MAX_BUF_SIZE        256
#define MAX_JSON_TOKENS     100
#define DEF_TIMEOUT         2.0
#define DEF_TIMEOUT_INIT    30.0
#define DEF_TIMEOUT_ARM     55.0
#define DEF_TIMEOUT_CMD     5.0
#define CHUNK_SIZE          (MAX_MESSAGE_SIZE-512)

#define ID_STR          "$id"
#define ID_LEN          3

#define API_VERSION     "1.0.4"
#define EOL             "\r\n"      // End of Line
#define EOH EOL         EOL         // End of Header
#define EOH_LEN         4           // End of Header Length

#define DATA_NATIVE "application/json; charset=utf-8"
#define DATA_TIFF   "application/tiff"
#define DATA_HDF5   "application/hdf5"
#define DATA_HTML   "text/html"

#define REQUEST_GET\
    "GET %s%s HTTP/1.0" EOL \
    "Accept: " DATA_NATIVE EOH

#define REQUEST_PUT\
    "PUT %s%s HTTP/1.0" EOL \
    "Accept-Encoding: identity" EOL\
    "Content-Type: " DATA_NATIVE EOL \
    "Content-Length: %u" EOH

#define REQUEST_HEAD\
    "HEAD %s%s HTTP/1.0" EOH

#define REQUEST_GET_PARTIAL\
    "GET %s%s HTTP/1.0" EOL \
    "Range: bytes=%lu-%lu" EOH

/** Subsystems */
typedef enum
{
    SSAPIVersion,
    SSDetConfig,
    SSDetStatus,
    SSFWConfig,
    SSFWStatus,
    SSCommand,
    SSData,

    SSCount,
} eigerSys;

typedef enum
{
    FWDisabled,
    FWEnabled,

    FWModeCount
} eigerFWMode;

typedef enum {
    TMInternal,         // INTS: One trigger multiple frames
    TMExternal,         // EXTS: One trigger multiple frames
    TMExternalMultiple, // EXTE" One trigger per frame, trigger width=exposure

    TMCount
} eigerTriggerMode;

static const char *eigerSysStr [SSCount] = {
    "/detector/api/version",
    "/detector/api/"   API_VERSION "/config/",
    "/detector/api/"   API_VERSION "/status/",
    "/filewriter/api/" API_VERSION "/config/",
    "/filewriter/api/" API_VERSION "/status/",
    "/detector/api/"   API_VERSION "/command/",
    "/data/",
};

static const char *eigerFWModeStr [FWModeCount] = {
    "disabled", "enabled"
};

static const char *eigerTMStr [TMCount] = {
    "ints", "exts", "exte"
};

struct response
{
    int code;
    char *data;
    size_t size;
    size_t contentLength;
};

static const char *driverName = "eigerDetector";

/* FileWriter Config Parameters */
#define EigerFWClearString              "CLEAR"
#define EigerFWCompressionString        "COMPRESSION"
#define EigerFWImageNrStartString       "IMAGE_NR_START"
#define EigerFWModeString               "MODE"
#define EigerFWNamePatternString        "NAME_PATTERN"
#define EigerFWNImgsPerFileString       "NIMAGES_PER_FILE"

/* Detector Config Parameters */
#define EigerBeamXString                "BEAM_X"
#define EigerBeamYString                "BEAM_Y"
#define EigerDetDistString              "DET_DIST"
#define EigerFlatfieldString            "FLATFIELD_APPLIED"
#define EigerPhotonEnergyString         "PHOTON_ENERGY"
#define EigerThresholdString            "THRESHOLD"
#define EigerWavelengthString           "WAVELENGTH"

/* Status */
#define EigerThTemp0String              "TH_TEMP_0"
#define EigerThHumid0String             "TH_HUMID_0"

/* Other */
#define EigerArmedString                "ARMED"
#define EigerSequenceIdString           "SEQ_ID"

/** Driver for Dectris Eiger pixel array detectors using their REST server */
class eigerDetector : public ADDriver
{
public:
    eigerDetector(const char *portName, const char *serverPort, int maxBuffers,
            size_t maxMemory, int priority, int stackSize);

    /* These are the methods that we override from ADDriver */
    virtual asynStatus writeInt32(asynUser *pasynUser, epicsInt32 value);
    virtual asynStatus writeFloat64(asynUser *pasynUser, epicsFloat64 value);
    virtual asynStatus writeOctet(asynUser *pasynUser, const char *value,
                                    size_t nChars, size_t *nActual);
    void report(FILE *fp, int details);

    /* This should be private but is called from C so must be public */
    void eigerTask();

protected:
    /* FileWriter Config Parameters */
    int EigerFWClear;
    #define FIRST_EIGER_PARAM EigerFWClear
    int EigerFWCompression;
    int EigerFWImageNrStart;
    int EigerFWMode;
    int EigerFWNamePattern;
    int EigerFWNImgsPerFile;

    /* Detector Config Parameters */
    int EigerBeamX;
    int EigerBeamY;
    int EigerDetDist;
    int EigerFlatfield;
    int EigerPhotonEnergy;
    int EigerThreshold;
    int EigerWavelength;

    /* Detector Status Parameters */
    int EigerThTemp0;
    int EigerThHumid0;

    /* Other parameters */
    int EigerArmed;
    int EigerSequenceId;
    #define LAST_EIGER_PARAM EigerSequenceId

private:
    epicsEventId startEventId;
    epicsEventId stopEventId;
    char toServer[MAX_MESSAGE_SIZE];
    char fromServer[MAX_MESSAGE_SIZE];
    asynUser *pasynUserServer;

    /* These are the methods that are new to this class */

    /*
     * Basic HTTP communication
     */
    asynStatus doRequest  (size_t requestSize, struct response *response,
            double timeout = DEF_TIMEOUT);

    /*
     * Get/set parameter value (string form)
     */
    asynStatus get (eigerSys sys, const char *param, char *value, size_t len,
            double timeout = DEF_TIMEOUT);
    asynStatus put (eigerSys sys, const char *param, const char *value,
            size_t len, double timeout = DEF_TIMEOUT);
    asynStatus parsePutResponse (struct response response);

    /*
     * Nice wrappers to set/get parameters
     */
    asynStatus getString  (eigerSys sys, const char *param, char *value,
            size_t len);
    asynStatus getInt     (eigerSys sys, const char *param, int *value);
    asynStatus getDouble  (eigerSys sys, const char *param, double *value);
    asynStatus getBool    (eigerSys sys, const char *param, bool *value);

    asynStatus getStringP (eigerSys sys, const char *param, int dest);
    asynStatus getIntP    (eigerSys sys, const char *param, int dest);
    asynStatus getDoubleP (eigerSys sys, const char *param, int dest);
    asynStatus getBoolP   (eigerSys sys, const char *param, int dest);

    asynStatus putString  (eigerSys sys, const char *param, const char *value);
    asynStatus putInt     (eigerSys sys, const char *param, int value);
    asynStatus putDouble  (eigerSys sys, const char *param, double value);
    asynStatus putBool    (eigerSys sys, const char *param, bool value);

    /*
     * Send a command to the detector
     */
    asynStatus command (const char *name, double timeout = DEF_TIMEOUT_CMD);

    /*
     * File getters
     */
    asynStatus getFileSize   (const char *remoteFile, size_t *len);
    asynStatus getFile       (const char *remoteFile, char **data, size_t *len);
    asynStatus getMasterFile (int sequenceId, char **data, size_t *len);
    asynStatus getDataFile   (int sequenceId, int nr, char **data, size_t *len);

    /*
     * Arm, trigger and disarm
     */
    asynStatus capture (void);

    /*
     * Download detector files locally and, at the same time, publish as
     * NDArrays
     */
    asynStatus downloadAndPublish (void);

    /*
     * HDF5 helpers
     */
    asynStatus readH5Attr   (hid_t entry, const char *name, int *value);
    asynStatus parseH5File  (char *buf, size_t len);
    asynStatus fillNDArrays (hid_t dId, size_t nimages);

    /*
     * Read some detector status parameters
     */
    asynStatus eigerStatus (void);
};

#define NUM_EIGER_PARAMS ((int)(&LAST_EIGER_PARAM - &FIRST_EIGER_PARAM + 1))

static void eigerTaskC (void *drvPvt)
{
    eigerDetector *pPvt = (eigerDetector *)drvPvt;
    pPvt->eigerTask();
}

/** Constructor for Eiger driver; most parameters are simply passed to
  * ADDriver::ADDriver.
  * After calling the base class constructor this method creates a thread to
  * collect the detector data, and sets reasonable default values for the
  * parameters defined in this class, asynNDArrayDriver, and ADDriver.
  * \param[in] portName The name of the asyn port driver to be created.
  * \param[in] serverPort The name of the asyn port previously created with
  *            drvAsynIPPortConfigure to communicate with server.
  * \param[in] portName The name of the asyn port driver to be created.
  * \param[in] maxBuffers The maximum number of NDArray buffers that the
  *            NDArrayPool for this driver is allowed to allocate. Set this to
  *            -1 to allow an unlimited number of buffers.
  * \param[in] maxMemory The maximum amount of memory that the NDArrayPool for
  *            this driver is allowed to allocate. Set this to -1 to allow an
  *            unlimited amount of memory.
  * \param[in] priority The thread priority for the asyn port driver thread if
  *            ASYN_CANBLOCK is set in asynFlags.
  * \param[in] stackSize The stack size for the asyn port driver thread if
  *            ASYN_CANBLOCK is set in asynFlags.
  */
eigerDetector::eigerDetector (const char *portName, const char *serverPort,
        int maxBuffers, size_t maxMemory, int priority,
        int stackSize)

    : ADDriver(portName, 1, NUM_EIGER_PARAMS, maxBuffers, maxMemory,
               0, 0,             /* No interfaces beyond ADDriver.cpp */
               ASYN_CANBLOCK,    /* ASYN_CANBLOCK=1, ASYN_MULTIDEVICE=0 */
               1,                /* autoConnect=1 */
               priority, stackSize)
{
    int status = asynSuccess;
    const char *functionName = "eigerDetector";

    /* Connect to REST server */
    status = pasynOctetSyncIO->connect(serverPort, 0, &pasynUserServer, NULL);

    /* Create the epicsEvents for signaling to the eiger task when acquisition
     * starts and stops */
    startEventId = epicsEventCreate(epicsEventEmpty);
    if (!startEventId)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s epicsEventCreate failure for start event\n",
                driverName, functionName);
        return;
    }

    stopEventId = epicsEventCreate(epicsEventEmpty);
    if (!stopEventId)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s epicsEventCreate failure for stop event\n",
                driverName, functionName);
        return;
    }

    createParam(EigerFWClearString,       asynParamInt32, &EigerFWClear);
    createParam(EigerFWCompressionString, asynParamInt32, &EigerFWCompression);
    createParam(EigerFWImageNrStartString,asynParamInt32, &EigerFWImageNrStart);
    createParam(EigerFWModeString,        asynParamInt32, &EigerFWMode);
    createParam(EigerFWNamePatternString, asynParamOctet, &EigerFWNamePattern);
    createParam(EigerFWNImgsPerFileString,asynParamInt32, &EigerFWNImgsPerFile);

    createParam(EigerBeamXString,         asynParamFloat64, &EigerBeamX);
    createParam(EigerBeamYString,         asynParamFloat64, &EigerBeamY);
    createParam(EigerDetDistString,       asynParamFloat64, &EigerDetDist);
    createParam(EigerFlatfieldString,     asynParamInt32,   &EigerFlatfield);
    createParam(EigerPhotonEnergyString,  asynParamFloat64, &EigerPhotonEnergy);
    createParam(EigerThresholdString,     asynParamFloat64, &EigerThreshold);
    createParam(EigerWavelengthString,    asynParamFloat64, &EigerWavelength);

    /* Detector Status Parameters */
    createParam(EigerThTemp0String,       asynParamFloat64, &EigerThTemp0);
    createParam(EigerThHumid0String,      asynParamFloat64, &EigerThHumid0);

    /* Other parameters */
    createParam(EigerArmedString,         asynParamInt32,   &EigerArmed);
    createParam(EigerSequenceIdString,    asynParamInt32,   &EigerSequenceId);

    status = asynSuccess;

    /* Set some default values for parameters */
    char desc[MAX_BUF_SIZE] = "";
    char *manufacturer, *space, *model;

    if(getString(SSDetConfig, "description", desc, sizeof(desc)))
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_WARNING,
                "%s:%s Eiger seems to be uninitialized\n"
                "Initializing... (may take a while)\n",
                driverName, functionName);

        if(command("initialize", DEF_TIMEOUT_INIT))
        {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s:%s Eiger FAILED TO INITIALIZE\n",
                    driverName, functionName);
            return;
        }

        asynPrint(pasynUserSelf, ASYN_TRACE_WARNING,
                    "%s:%s Eiger initialized\n",
                    driverName, functionName);
    }

    status = getString(SSDetConfig, "description", desc, sizeof(desc));

    // Assume 'description' is of the form 'Dectris Eiger 1M'
    space = strchr(desc, ' ');
    *space = '\0';
    manufacturer = desc;
    model = space + 1;

    status |= setStringParam (ADManufacturer, manufacturer);
    status |= setStringParam (ADModel, model);

    int maxSizeX, maxSizeY;
    status |= getInt(SSDetConfig, "x_pixels_in_detector", &maxSizeX);
    status |= getInt(SSDetConfig, "y_pixels_in_detector", &maxSizeY);

    status |= setIntegerParam(ADMaxSizeX, maxSizeX);
    status |= setIntegerParam(ADMaxSizeY, maxSizeY);
    status |= setIntegerParam(ADSizeX, maxSizeX);
    status |= setIntegerParam(ADSizeY, maxSizeY);
    status |= setIntegerParam(NDArraySizeX, maxSizeX);
    status |= setIntegerParam(NDArraySizeY, maxSizeY);

    // Only internal trigger is supported at this time
    status |= setIntegerParam(ADTriggerMode, TMInternal);
    status |= putString(SSDetConfig, "trigger_mode", eigerTMStr[TMInternal]);

    char fwMode[MAX_BUF_SIZE];
    status |= getString(SSFWConfig, "mode", fwMode, sizeof(fwMode));
    status |= setIntegerParam(EigerFWMode, (int)(fwMode[0] == 'e'));

    status |= getDoubleP(SSDetConfig, "count_time",       ADAcquireTime);
    status |= getDoubleP(SSDetConfig, "frame_time",       ADAcquirePeriod);
    status |= getIntP   (SSDetConfig, "nimages",          ADNumImages);
    status |= getDoubleP(SSDetConfig, "photon_energy",    EigerPhotonEnergy);
    status |= getDoubleP(SSDetConfig, "threshold_energy", EigerThreshold);

    status |= getBoolP  (SSFWConfig, "compression_enabled",EigerFWCompression);
    status |= getIntP   (SSFWConfig, "image_nr_start",     EigerFWImageNrStart);
    status |= getStringP(SSFWConfig, "name_pattern",       EigerFWNamePattern);
    status |= getIntP   (SSFWConfig, "nimages_per_file",   EigerFWNImgsPerFile);

    status |= getDoubleP(SSDetConfig, "beam_center_x",     EigerBeamX);
    status |= getDoubleP(SSDetConfig, "beam_center_y",     EigerBeamY);
    status |= getDoubleP(SSDetConfig, "detector_distance", EigerDetDist);
    status |= getBoolP  (SSDetConfig, "flatfield_correction_applied",
            EigerFlatfield);
    status |= getDoubleP(SSDetConfig, "threshold_energy",  EigerThreshold);
    status |= getDoubleP(SSDetConfig, "wavelength",        EigerWavelength);

    status |= getDoubleP(SSDetStatus, "board_000/th0_temp",     EigerThTemp0);
    status |= getDoubleP(SSDetStatus, "board_000/th0_humidity", EigerThHumid0);

    status |= setIntegerParam(NDArraySize, 0);
    status |= setIntegerParam(NDDataType,  NDUInt32);
    status |= setIntegerParam(ADImageMode, ADImageMultiple);
    status |= setIntegerParam(EigerArmed,  0);
    status |= setIntegerParam(EigerSequenceId, 0);

    status |= setStringParam(NDFileTemplate, "%s");

    callParamCallbacks();

    // Auto Summation should always be true (Eiger API Reference v1.1pre)
    status |= putBool(SSDetConfig, "auto_summation", true);

    if (status)
    {
        printf("%s: unable to set camera parameters\n", functionName);
        return;
    }

    /* Create the thread that updates the images */
    status = (epicsThreadCreate("eigerDetTask", epicsThreadPriorityMedium,
            epicsThreadGetStackSize(epicsThreadStackMedium),
            (EPICSTHREADFUNC)eigerTaskC, this) == NULL);

    if (status)
    {
        printf("%s:%s epicsThreadCreate failure for image task\n",
            driverName, functionName);
        return;
    }
}

/** Called when asyn clients call pasynInt32->write().
  * This function performs actions for some parameters, including ADAcquire,
  * ADTriggerMode, etc.
  * For all parameters it sets the value in the parameter library and calls any
  * registered callbacks..
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Value to write. */
asynStatus eigerDetector::writeInt32 (asynUser *pasynUser, epicsInt32 value)
{
    int function = pasynUser->reason;
    asynStatus status = asynSuccess;
    const char *functionName = "writeInt32";
    int adstatus;

    if (function == EigerFWClear)
        status = putInt(SSFWConfig, "clear", 1);
    else if (function == EigerFWCompression)
        status = putBool(SSFWConfig, "compression_enabled", (bool)value);
    else if (function == EigerFWImageNrStart)
        status = putInt(SSFWConfig, "image_nr_start", value);
    else if (function == EigerFWMode)
        status = putString(SSFWConfig, "mode", eigerFWModeStr[value]);
    else if (function == EigerFWNImgsPerFile)
        status = putInt(SSFWConfig, "nimages_per_file", value);
    else if (function == EigerFlatfield)
        status = putBool(SSDetConfig, "flatfield_correction_applied", (bool)value);
    else if (function == ADTriggerMode)
    {
        value = TMInternal; // only supported trigger mode
        //status = putString(SSDetConfig, "trigger_mode", eigerTMStr[value]);
    }
    else if (function == ADNumImages)
        status = putInt(SSDetConfig, "nimages", value);
    else if (function == ADReadStatus)
        status = eigerStatus();
    else if (function == ADAcquire)
    {
        getIntegerParam(ADStatus, &adstatus);

        if (value && (adstatus == ADStatusIdle || adstatus == ADStatusError ||
                adstatus == ADStatusAborted))
        {
            setStringParam(ADStatusMessage, "Acquiring data");
            setIntegerParam(ADStatus, ADStatusAcquire);
        }

        if (!value && adstatus == ADStatusAcquire)
        {
            setStringParam(ADStatusMessage, "Acquisition aborted");
            setIntegerParam(ADStatus, ADStatusAborted);
        }
    }
    else if(function < FIRST_EIGER_PARAM)
        status = ADDriver::writeInt32(pasynUser, value);

    if(status)
    {
        asynPrint(pasynUser, ASYN_TRACE_ERROR,
              "%s:%s: error, status=%d function=%d, value=%d\n",
              driverName, functionName, status, function, value);
        return status;
    }

    status = setIntegerParam(function, value);
    callParamCallbacks();

    // Effectively start/stop acquisition if that's the case
    if (function == ADAcquire)
    {
        if (value && (adstatus == ADStatusIdle || adstatus == ADStatusError ||
                adstatus == ADStatusAborted))
        {
            epicsEventSignal(this->startEventId);
        }
        else if (!value && (adstatus == ADStatusAcquire))
        {
            epicsEventSignal(this->stopEventId);
        }
    }

    if (status)
        asynPrint(pasynUser, ASYN_TRACE_ERROR,
              "%s:%s: error, status=%d function=%d, value=%d\n",
              driverName, functionName, status, function, value);
    else
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER,
              "%s:%s: function=%d, value=%d\n",
              driverName, functionName, function, value);

    return status;
}

/** Called when asyn clients call pasynFloat64->write().
  * This function performs actions for some parameters, including ADAcquireTime,
  * ADGain, etc.
  * For all parameters it sets the value in the parameter library and calls any
  * registered callbacks..
  * \param[in] pasynUser pasynUser structure that encodes the reason and
  *            address.
  * \param[in] value Value to write. */
asynStatus eigerDetector::writeFloat64 (asynUser *pasynUser, epicsFloat64 value)
{
    int function = pasynUser->reason;
    asynStatus status = asynSuccess;
    const char *functionName = "writeFloat64";

    if (function == EigerBeamX)
        status = putDouble(SSDetConfig, "beam_center_x", value);
    else if (function == EigerBeamY)
        status = putDouble(SSDetConfig, "beam_center_y", value);
    else if (function == EigerDetDist)
        status = putDouble(SSDetConfig, "detector_distance", value);
    else if (function == EigerPhotonEnergy)
        status = putDouble(SSDetConfig, "photon_energy", value);
    else if (function == EigerThreshold)
        status = putDouble(SSDetConfig, "threshold_energy", value);
    else if (function == EigerWavelength)
        status = putDouble(SSDetConfig, "wavelength", value);
    else if (function == ADAcquireTime)
        status = putDouble(SSDetConfig, "count_time", value);
    else if (function == ADAcquirePeriod)
        status = putDouble(SSDetConfig, "frame_time", value);
    else if (function < FIRST_EIGER_PARAM)
        status = ADDriver::writeFloat64(pasynUser, value);

    if (status)
    {
        asynPrint(pasynUser, ASYN_TRACE_ERROR,
              "%s:%s error, status=%d function=%d, value=%f\n",
              driverName, functionName, status, function, value);
    }
    else
    {
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER,
              "%s:%s: function=%d, value=%f\n",
              driverName, functionName, function, value);

        /* Do callbacks so higher layers see any changes */
        setDoubleParam(function, value);
        callParamCallbacks();
    }
    return status;
}

/** Called when asyn clients call pasynOctet->write().
  * This function performs actions for some parameters, including eigerBadPixelFile, ADFilePath, etc.
  * For all parameters it sets the value in the parameter library and calls any registered callbacks..
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Address of the string to write.
  * \param[in] nChars Number of characters to write.
  * \param[out] nActual Number of characters actually written. */
asynStatus eigerDetector::writeOctet (asynUser *pasynUser, const char *value,
                                    size_t nChars, size_t *nActual)
{
    int function = pasynUser->reason;
    asynStatus status = asynSuccess;
    const char *functionName = "writeOctet";

    if (function == EigerFWNamePattern)
        putString(SSFWConfig, "name_pattern", value);
    else if (function < FIRST_EIGER_PARAM)
        status = ADDriver::writeOctet(pasynUser, value, nChars, nActual);

    status = setStringParam(function, value);
    callParamCallbacks();

    if (status)
        asynPrint(pasynUser, ASYN_TRACE_ERROR,
                  "%s:%s: status=%d, function=%d, value=%s",
                  driverName, functionName, status, function, value);
    else
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER,
              "%s:%s: function=%d, value=%s\n",
              driverName, functionName, function, value);

    *nActual = nChars;
    return status;
}

/** Report status of the driver.
  * Prints details about the driver if details>0.
  * It then calls the ADDriver::report() method.
  * \param[in] fp File pointed passed by caller where the output is written to.
  * \param[in] details If >0 then driver details are printed.
  */
void eigerDetector::report (FILE *fp, int details)
{
    fprintf(fp, "Eiger detector %s\n", this->portName);
    if (details > 0) {
        int nx, ny, dataType;
        getIntegerParam(ADSizeX, &nx);
        getIntegerParam(ADSizeY, &ny);
        getIntegerParam(NDDataType, &dataType);
        fprintf(fp, "  NX, NY:            %d  %d\n", nx, ny);
        fprintf(fp, "  Data type:         %d\n", dataType);
    }
    /* Invoke the base class method */
    ADDriver::report(fp, details);
}

/** This thread controls acquisition, reads image files to get the image data,
  * and does the callbacks to send it to higher layers */
void eigerDetector::eigerTask (void)
{
    const char *functionName = "eigerTask";
    int status = asynSuccess;

    this->lock();

    for(;;)
    {
        int acquire;
        getIntegerParam(ADAcquire, &acquire);

        if (!acquire)
        {
            if (!status)
                setStringParam(ADStatusMessage, "Waiting for acquire command");

            callParamCallbacks();

            this->unlock(); // Wait for semaphore unlocked

            asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
                    "%s:%s: waiting for acquire to start\n",
                    driverName, functionName);

            status = epicsEventWait(this->startEventId);   // Wait for semaphore
            this->lock();
        }

        int filePathExists;
        checkPath();
        getIntegerParam(NDFilePathExists, &filePathExists);
        if(!filePathExists)
        {
            asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s:%s: invalid local File Path\n",
                    driverName, functionName);
            status = asynError;
            setIntegerParam(ADStatus, ADStatusError);
            setStringParam(ADStatusMessage, "Invalid file path");
            goto end;
        }

        /*
         * Acquire
         */
        if((status = capture()))
        {
            asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s:%s: underlying capture failed\n",
                    driverName, functionName);

            setIntegerParam(ADStatus, ADStatusError);
            goto end;
        }

        /*
         * Download and publish
         */

        int arrayCallbacks;
        getIntegerParam(NDArrayCallbacks, &arrayCallbacks);
        if (arrayCallbacks)
            if((status = downloadAndPublish()))
            {
                asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                        "%s:%s: underlying downloadAndPublish failed\n",
                        driverName, functionName);
                setIntegerParam(ADStatus, ADStatusError);
                setStringParam(ADStatusMessage, "Download failed");
            }

end:
        /* If everything was ok, set the status back to idle */
        int statusParam = 0;
        getIntegerParam(ADStatus, &statusParam);

        if (!status)
            setIntegerParam(ADStatus, ADStatusIdle);
        else if (statusParam != ADStatusAborted)
            setIntegerParam(ADStatus, ADStatusError);

        setIntegerParam(ADAcquire, 0);
        callParamCallbacks();
    }
}

asynStatus eigerDetector::doRequest (size_t requestSize,
        struct response *response, double timeout)
{
    const char *functionName = "doRequest";
    asynStatus status;
    size_t nwrite, nread;
    int eomReason;

    setStringParam(ADStringToServer, toServer);
    setStringParam(ADStringFromServer, "");
    callParamCallbacks();

    // Send request / get response
    if((status = pasynOctetSyncIO->writeRead(pasynUserServer,
            toServer, requestSize, fromServer, sizeof(fromServer),
            timeout, &nwrite, &nread, &eomReason)))
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s, send/recv failed\n[%s]\n",
                driverName, functionName, pasynUserServer->errorMessage);
        return status;
    }

    // Find Content-Length (useful for HEAD requests)
    char *contentLength = strcasestr(fromServer, "Content-Length");
    if(!contentLength)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s, malformed packet: no Content-Length\n",
                driverName, functionName);
        return asynError;
    }

    if(sscanf(contentLength, "%*s %lu", &response->contentLength) != 1)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s, malformed packet: couldn't parse Content-Length\n",
                driverName, functionName);
        return asynError;
    }

    // Find end of header
    char *eoh = strstr(fromServer, EOH);
    if(!eoh)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s, malformed packet: no End of Header\n",
                driverName, functionName);
        return asynError;
    }

    // Fill response
    if(sscanf(fromServer, "%*s %d", &response->code) != 1)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s, malformed packet: couldn't parse response code\n",
                driverName, functionName);
        return asynError;
    }

    response->data = eoh + EOH_LEN;
    response->size = nread - (size_t)(response->data-fromServer);

    if(response->size < sizeof(fromServer))
        response->data[response->size] = '\0';

    setStringParam(ADStringFromServer, fromServer);
    callParamCallbacks();

    return asynSuccess;
}

asynStatus eigerDetector::get (eigerSys sys, const char *param, char *value,
        size_t len, double timeout)
{
    const char *functionName = "get";
    const char *reqFmt = REQUEST_GET;
    const char *url = eigerSysStr[sys];
    asynStatus status;
    size_t reqSize;
    struct response response;

    reqSize = snprintf(toServer, sizeof(toServer), reqFmt, url, param);

    if((status = doRequest(reqSize, &response, timeout)))
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), request failed\n",
                driverName, functionName, param);
        return status;
    }

    if(response.code != 200)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), server returned error code %d\n",
                driverName, functionName, param, response.code);
        return asynError;
    }

    if(!value)
        return asynSuccess;

    struct json_token tokens[MAX_JSON_TOKENS];
    struct json_token *valueToken;

    if(parse_json(response.data, response.size, tokens, MAX_JSON_TOKENS) < 0)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), unable to parse response json\n[%.*s]\n",
                driverName, functionName, param, (int)response.size,
                response.data);
        return asynError;
    }

    valueToken = find_json_token(tokens, "value");

    if(valueToken == NULL)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), unable to find 'value' json field\n",
                driverName, functionName, param);
        return asynError;
    }

    if((size_t)valueToken->len > ((size_t)(len + 1)))
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), destination buffer is too short\n",
                driverName, functionName, param);
        return asynError;
    }

    memcpy((void*)value, (void*)valueToken->ptr, valueToken->len);
    value[valueToken->len] = '\0';

    return asynSuccess;
}

asynStatus eigerDetector::put (eigerSys sys, const char *param,
        const char *value, size_t len, double timeout)
{
    const char *functionName = "put";
    const char *reqFmt = REQUEST_PUT ;
    const char *url = eigerSysStr[sys];
    asynStatus status;
    size_t reqSize, remaining;
    struct response response;

    reqSize = snprintf(toServer, sizeof(toServer), reqFmt, url, param, len);
    remaining = sizeof(toServer) - reqSize;

    if(remaining < len)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), toServer buffer is not big enough\n",
                driverName, functionName, param);
        return asynError;
    }

    if(len && value)
    {
        memcpy(toServer + reqSize, value, len);
        reqSize += len;
        remaining -= len;
    }

    if(remaining)
        *(toServer+reqSize) = '\0';     // Prettify  ADStringToServer

    if((status = doRequest(reqSize, &response, timeout)))
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), request failed\n",
                driverName, functionName, param);
        return status;
    }

    if(response.code != 200)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), server returned error code %d\n",
                driverName, functionName, param, response.code);
        return asynError;
    }

    if(response.size && (status = parsePutResponse(response)))
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
            "%s::%s(%s), unable to parse response\n",
            driverName, functionName, param);
        return status;
    }

    return asynSuccess;
}

asynStatus eigerDetector::parsePutResponse(struct response response)
{
    // Try to parse the response
    // Two possibilities:
    //   Response to PUT to a parameter: list of changed values
    //   Response to the arm command: sequence id
    const char *functionName = "parsePutResponse";

    // Copy response data locally (may be overwritten by GETs)
    char responseData[response.size];
    memcpy(responseData, response.data, response.size);

    struct json_token tokens[MAX_JSON_TOKENS];
    if(parse_json(responseData, response.size, tokens, MAX_JSON_TOKENS) < 0)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s, unable to parse response json\n",
                driverName, functionName);
        return asynError;
    }

    if(tokens[0].type == JSON_TYPE_OBJECT)  // sequence id
    {
        struct json_token *seqIdToken = find_json_token(tokens, "sequence id");

        if(!seqIdToken)
        {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s::%s, unable to find 'sequence_id' token\n",
                    driverName, functionName);
            return asynError;
        }

        int seqId;
        if(sscanf(seqIdToken->ptr, "%d", &seqId) != 1)
        {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s::%s, unable to parse 'sequence_id' token\n",
                    driverName, functionName);
            return asynError;
        }

        setIntegerParam(EigerSequenceId, seqId);
        callParamCallbacks();
    }
    else if(tokens[0].type == JSON_TYPE_ARRAY)  // list of parameter names
    {
        for(int i = 1; i <= tokens[0].num_desc; ++i)
        {
            if(!strncmp(tokens[i].ptr, "count_time", tokens[i].len))
                getDoubleP (SSDetConfig, "count_time", ADAcquireTime);
            else if(!strncmp(tokens[i].ptr, "frame_time", tokens[i].len))
                getDoubleP (SSDetConfig, "frame_time", ADAcquirePeriod);
            else if(!strncmp(tokens[i].ptr, "nimages", tokens[i].len))
                getIntP (SSDetConfig, "nimages", ADNumImages);
            else if(!strncmp(tokens[i].ptr, "photon_energy", tokens[i].len))
                getDoubleP(SSDetConfig, "photon_energy", EigerPhotonEnergy);
            else if(!strncmp(tokens[i].ptr, "beam_center_x", tokens[i].len))
                getDoubleP(SSDetConfig, "beam_center_x", EigerBeamX);
            else if(!strncmp(tokens[i].ptr, "beam_center_y", tokens[i].len))
                getDoubleP(SSDetConfig, "beam_center_y", EigerBeamY);
            else if(!strncmp(tokens[i].ptr, "detector_distance", tokens[i].len))
                getDoubleP(SSDetConfig, "detector_distance", EigerDetDist);
            else if(!strncmp(tokens[i].ptr, "threshold_energy", tokens[i].len))
                getDoubleP(SSDetConfig, "threshold_energy", EigerThreshold);
            else if(!strncmp(tokens[i].ptr, "wavelength", tokens[i].len))
                getDoubleP(SSDetConfig, "wavelength", EigerWavelength);
        }
        callParamCallbacks();
    }
    else
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s, unexpected json token type\n",
                driverName, functionName);
        return asynError;
    }

    return asynSuccess;
}

asynStatus eigerDetector::getString (eigerSys sys, const char *param,
        char *value, size_t len)
{
    return get(sys, param, value, len);
}

asynStatus eigerDetector::getInt (eigerSys sys, const char *param, int *value)
{
    const char *functionName = "getInt";
    asynStatus status;
    char buf[MAX_BUF_SIZE];

    if((status = get(sys, param, buf, sizeof(buf))))
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s::%s(%s), underlying get failed\n",
                    driverName, functionName, param);
        return status;
    }

    if(sscanf(buf, "%d", value) != 1)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), couldn't parse '%s' as integer\n",
                driverName, functionName, param, buf);
        return asynError;
    }

    return asynSuccess;
}

asynStatus eigerDetector::getDouble (eigerSys sys, const char *param,
        double *value)
{
    const char *functionName = "getDouble";
    asynStatus status;
    char buf[MAX_BUF_SIZE];

    if((status = get(sys, param, buf, sizeof(buf))))
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), underlying get failed\n",
                driverName, functionName, param);
        return status;
    }

    if(sscanf(buf, "%lf", value) != 1)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), couldn't parse '%s' as double\n",
                driverName, functionName, param, buf);
        return asynError;
    }

    return asynSuccess;
}

asynStatus eigerDetector::getBool (eigerSys sys, const char *param, bool *value)
{
    const char *functionName = "getBool";
    asynStatus status;
    char buf[MAX_BUF_SIZE];

    if((status = get(sys, param, buf, sizeof(buf))))
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), underlying get failed\n",
                driverName, functionName, param);
        return status;
    }

    *value = buf[0] == 't';

    return asynSuccess;
}

asynStatus eigerDetector::getStringP (eigerSys sys, const char *param, int dest)
{
    int status;
    char value[MAX_BUF_SIZE];

    status = getString(sys, param, value, sizeof(value)) |
            setStringParam(dest, value);
    return (asynStatus)status;
}

asynStatus eigerDetector::getIntP (eigerSys sys, const char *param, int dest)
{
    int status;
    int value;

    status = getInt(sys, param, &value) | setIntegerParam(dest,value);
    return (asynStatus)status;
}

asynStatus eigerDetector::getDoubleP (eigerSys sys, const char *param, int dest)
{
    int status;
    double value;

    status = getDouble(sys, param, &value) | setDoubleParam(dest, value);
    return (asynStatus)status;
}

asynStatus eigerDetector::getBoolP (eigerSys sys, const char *param, int dest)
{
    int status;
    bool value;

    status = getBool(sys, param, &value) | setIntegerParam(dest, (int)value);
    return (asynStatus)status;
}

asynStatus eigerDetector::putString (eigerSys sys, const char *param,
        const char *value)
{
    const char *functionName = "putString";
    char buf[MAX_BUF_SIZE];
    size_t len = sprintf(buf, "{\"value\": \"%s\"}", value);
    asynStatus status = put(sys, param, buf, len);

    if(status)
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), underlying put failed\n",
                driverName, functionName, param);

    return status;
}

asynStatus eigerDetector::putInt (eigerSys sys, const char *param, int value)
{
    const char *functionName = "putInt";
    char buf[MAX_BUF_SIZE];
    size_t len = sprintf(buf, "{\"value\":%d}", value);
    asynStatus status = put(sys, param, buf, len);

    if(status)
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), underlying put failed\n",
                driverName, functionName, param);

    return status;
}

asynStatus eigerDetector::putBool (eigerSys sys, const char *param, bool value)
{
    const char *functionName = "putBool";
    char buf[MAX_BUF_SIZE];
    size_t len = sprintf(buf, "{\"value\": %s}", value ? "true" : "false");
    asynStatus status = put(sys, param, buf, len);

    if(status)
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), underlying put failed\n",
                driverName, functionName, param);

    return status;
}

asynStatus eigerDetector::putDouble (eigerSys sys, const char *param,
        double value)
{
    const char *functionName = "putDouble";
    char buf[MAX_BUF_SIZE];
    size_t len = sprintf(buf, "{\"value\": %lf}", value);
    asynStatus status = put(sys, param, buf, len);

    if(status)
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), underlying put failed\n",
                driverName, functionName, param);

    return status;
}

asynStatus eigerDetector::command (const char *name, double timeout)
{
    const char *functionName = "command";
    asynStatus status = put(SSCommand, name, NULL, 0, timeout);

    if(status)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
            "%s::%s(%s), underlying put failed\n",
            driverName, functionName, name);
    }

    return status;
}

asynStatus eigerDetector::getFileSize (const char *remoteFile, size_t *len)
{
    const char *functionName = "getFileSize";
    const char *reqFmt = REQUEST_HEAD;
    const char *url = eigerSysStr[SSData];
    asynStatus status;
    size_t reqSize;
    struct response response;

    size_t retries = GET_FILE_RETRIES;

    reqSize = snprintf(toServer, sizeof(toServer), reqFmt, url, remoteFile);

    while(retries > 0)
    {
        if((status = doRequest(reqSize, &response)))
        {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s::%s(%s), HEAD request failed\n",
                    driverName, functionName, remoteFile);
            return status;
        }

        if(response.code == 404)
        {
            epicsThreadSleep(.01);
            retries -= 1;
            continue;
        }

        if(response.code != 200)
        {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s::%s(%s), server returned error code %d\n",
                    driverName, functionName, remoteFile, response.code);
            return asynError;
        }
        break;
    }

    if(!retries)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), server returned error code %d %d times\n",
                driverName, functionName, remoteFile, response.code,
                GET_FILE_RETRIES);
        return asynError;
    }

    *len = response.contentLength;
    return asynSuccess;
}

asynStatus eigerDetector::getFile (const char *remoteFile, char **data,
        size_t *len)
{
    const char *functionName = "getFile";
    const char *reqFmt = REQUEST_GET_PARTIAL;
    const char *url = eigerSysStr[SSData];
    asynStatus status = asynSuccess;
    size_t reqSize;
    struct response response;
    size_t remaining;
    char *dataPtr;

    *len = 0;

    if((status = getFileSize(remoteFile, &remaining)))
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), underlying getFileSize failed\n",
                driverName, functionName, remoteFile);
        return asynError;
    }

    char fullFileName[MAX_FILENAME_LEN];
    setStringParam(NDFileName, remoteFile);
    setStringParam(NDFileTemplate, "%s%s");
    createFileName(sizeof(fullFileName), fullFileName);
    setStringParam(NDFullFileName, fullFileName);
    callParamCallbacks();

    FILE *fhandle = fopen(fullFileName, "wb");
    if(!fhandle)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), unable to open file to be written\n[%s]\n",
                driverName, functionName, remoteFile, fullFileName);
        return asynError;
    }

    *data = (char*)malloc(remaining);
    if(!*data)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s(%s), malloc(%lu) failed\n",
                driverName, functionName, remoteFile, remaining);
        status = asynError;
        goto closeFile;
    }

    printf("Will download %s: %lu bytes\n", remoteFile, remaining);

    dataPtr = *data;
    while(remaining)
    {
        reqSize = snprintf(toServer, sizeof(toServer), reqFmt, url, remoteFile,
                *len, *len + CHUNK_SIZE - 1);

        if((status = doRequest(reqSize, &response)))
        {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s::%s(%s), partial GET request failed\n",
                    driverName, functionName, remoteFile);
            status = asynError;
            break;
        }

        if(response.code != 206)
        {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s::%s(%s), server returned error code %d\n",
                    driverName, functionName, remoteFile, response.code);
            status = asynError;
            break;
        }

        memcpy(dataPtr, response.data, response.size);
        if(fwrite(response.data, 1, response.size, fhandle) < response.size)
        {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s::%s(%s), failed to write chunk to local file\n",
                    driverName, functionName, remoteFile);
            status = asynError;
            break;

        }

        dataPtr += response.size;
        *len += response.size;
        remaining -= response.size;
    }

    if(status)
    {
        free(*data);
        *data = NULL;
    }

closeFile:
    fclose(fhandle);
    return status;
}

asynStatus eigerDetector::getMasterFile (int sequenceId, char **data,
        size_t *len)
{
    const char *functionName = "getMasterFile";
    asynStatus status;
    char pattern[MAX_BUF_SIZE];
    getStringParam(EigerFWNamePattern, sizeof(pattern), pattern);
    char *id = strstr(pattern, ID_STR);
    *id = '\0';

    char fileName[MAX_BUF_SIZE];
    sprintf(fileName, "%s%d%s_master.h5", pattern, sequenceId,
            id + ID_LEN);

    if((status = getFile(fileName, data, len)))
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
            "%s::%s, underlying getFile(%s) failed\n",
            driverName, functionName, fileName);
        return status;
    }

    return asynSuccess;
}

asynStatus eigerDetector::getDataFile (int sequenceId, int nr, char **data,
        size_t *len)
{
    const char *functionName = "getDataFile";
    asynStatus status;
    char pattern[MAX_BUF_SIZE];
    getStringParam(EigerFWNamePattern, sizeof(pattern), pattern);
    char *id = strstr(pattern, ID_STR);
    *id = '\0';

    char filename[MAX_BUF_SIZE];
    sprintf(filename, "%s%d%s_data_%06d.h5", pattern, sequenceId,
            id + ID_LEN, nr);

    if((status = getFile(filename, data, len)))
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
            "%s::%s, underlying getFile(%s) failed\n",
            driverName, functionName, filename);
        return status;
    }

    return asynSuccess;
}

asynStatus eigerDetector::capture (void)
{
    const char *functionName = "capture";
    asynStatus status;
    asynStatus retStatus = asynSuccess;

    double acquirePeriod, triggerTimeout;
    int numImages;

    getDoubleParam(ADAcquirePeriod, &acquirePeriod);
    getIntegerParam(ADNumImages, &numImages);
    triggerTimeout = acquirePeriod*numImages + 10.0;

    // Open shutter
    setShutter(1);

    setIntegerParam(ADStatus, ADStatusAcquire);
    setStringParam(ADStatusMessage, "Arming the detector (takes a while)");
    callParamCallbacks();

    // Arm the detector
    if((status = command("arm", DEF_TIMEOUT_ARM)))
    {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s: failed to arm the detector\n",
                driverName, functionName);

        retStatus = status;
        setIntegerParam(ADStatus, ADStatusError);
        setStringParam(ADStatusMessage, "Failed to arm the detector");
        goto closeShutter;
    }

    // Set armed flag
    setIntegerParam(EigerArmed, 1);
    setStringParam(ADStatusMessage, "Triggering the detector");
    callParamCallbacks();

    // Actually acquire the image(s)
    if((status = command("trigger", triggerTimeout)))
    {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s: failed to trigger the detector\n",
                driverName, functionName);

        retStatus = status;
        setIntegerParam(ADStatus, ADStatusError);
        setStringParam(ADStatusMessage, "Failed to trigger the detector");
        // continue to disarm
    }

    // Image(s) acquired or aborted. Disarm the detector
    if((status = command("disarm")))
    {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s: failed to disarm the detector\n",
                driverName, functionName);

        retStatus = status;
        setIntegerParam(ADStatus, ADStatusError);
        setStringParam(ADStatusMessage, "Failed to disarm the detector");
        goto closeShutter;
    }

    setIntegerParam(EigerArmed, 0);

closeShutter:
    setShutter(0);
    callParamCallbacks();
    return retStatus;
}

asynStatus eigerDetector::downloadAndPublish (void)
{
    const char *functionName = "downloadAndPublish";
    asynStatus status = asynSuccess;

    int numImages, sequenceId, numImagesPerFile, nrStart, nFiles;

    getIntegerParam(ADNumImages,         &numImages);
    getIntegerParam(EigerSequenceId,     &sequenceId);
    getIntegerParam(EigerFWNImgsPerFile, &numImagesPerFile);
    getIntegerParam(EigerFWImageNrStart, &nrStart);

    setIntegerParam(ADStatus, ADStatusReadout);

    // Wait for file to exist
    // TODO: Is this the best way?
    char buf[MAX_BUF_SIZE];
    do
    {
        getString(SSFWStatus, "state", buf, sizeof(buf));
    }while(buf[0] == 'a');

    setStringParam(ADStatusMessage, "Downloading master file");
    callParamCallbacks();

    char *master;
    size_t masterLen;
    if((status = getMasterFile(sequenceId, &master, &masterLen)))
    {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s: failed to get master file\n",
                driverName, functionName);
        return status;
    }

    if(master)
    {
        free(master);
        master = NULL;
    }

    setStringParam(ADStatusMessage, "Downloading data files");
    callParamCallbacks();

    nFiles = (int) ceil(((double)numImages)/((double)numImagesPerFile));

    for(int i = 0; i < nFiles; ++i)
    {
        char *data;
        size_t dataLen;

        if((status = getDataFile(sequenceId, i + nrStart, &data, &dataLen)))
        {
            asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s:%s: failed to get data file %d\n",
                    driverName, functionName, i + nrStart);
            break;
        }
        printf("got data file %d, %lu bytes\n", i+nrStart, dataLen);

        // Copy from memory to NDArrays
        if((status = parseH5File(data, dataLen)))
        {
            asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s:%s: underlying parseH5File failed\n",
                    driverName, functionName);
        }

        if(data)
        {
            free(data);
            data = NULL;
        }

        if(status)
            break;
    }

    return status;
}

asynStatus eigerDetector::readH5Attr (hid_t entry, const char *name, int *value)
{
    const char *functionName = "readH5Attr";
    asynStatus status = asynSuccess;

    htri_t exists = H5Aexists_by_name(entry, "data", name, H5P_DEFAULT);
    if(exists <= 0)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s couldn't find '%s' attribute\n",
                driverName, functionName, name);
        return asynError;
    }

    hid_t id = H5Aopen_by_name  (entry, "data", name, H5P_DEFAULT, H5P_DEFAULT);
    if(id < 0)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s couldn't open '%s' attribute\n",
                driverName, functionName, name);
        return asynError;
    }

    hid_t type = H5Aget_type(id);
    if(H5Aread (id, type, value) < 0)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s couldn't read '%s' attribute\n",
                driverName, functionName, name);
        status = asynError;
    }

    H5Aclose(id);
    return status;
}

asynStatus eigerDetector::parseH5File (char *buf, size_t bufLen)
{
    const char *functionName = "imgCopy";
    asynStatus status = asynSuccess;

    hid_t fileId, groupId, dataId;

    unsigned flags = H5LT_FILE_IMAGE_DONT_COPY | H5LT_FILE_IMAGE_DONT_RELEASE;

    // Open h5 file from memory
    fileId = H5LTopen_file_image(buf, bufLen, flags);
    if(fileId < 0)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s unable to open memory as file\n",
                driverName, functionName);
        return asynError;
    }

    //Access /entry group inside h5
    groupId = H5Gopen2(fileId, "/entry", H5P_DEFAULT);
    if(groupId < 0)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s unable to open 'entry' group\n",
                driverName, functionName);
        status = asynError;
        goto closeFile;
    }

    int image_nr_low;
    if((status = readH5Attr(groupId, "image_nr_low", &image_nr_low)))
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s underlying readH5Attr failed\n",
                driverName, functionName);
        goto closeGroup;
    }

    int image_nr_high;
    if((status = readH5Attr(groupId, "image_nr_high", &image_nr_high)))
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s underlying readH5Attr failed\n",
                driverName, functionName);
        goto closeGroup;
    }

    // Access dataset 'data'
    dataId = H5Dopen2(groupId, "data", H5P_DEFAULT);
    if(dataId < 0)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s unable to open 'data' dataset\n",
                driverName, functionName);
        status = asynError;
        goto closeGroup;
    }

    if((status = fillNDArrays(dataId, (image_nr_high-image_nr_low)+1)))
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s underlying fillNDArrays failed\n",
                driverName, functionName);
    }

    H5Dclose(dataId);
closeGroup:
    H5Gclose(groupId);
closeFile:
    H5Fclose(fileId);
    return status;
}

asynStatus eigerDetector::fillNDArrays (hid_t dId, size_t nimages)
{
    const char *functionName = "fillArray";
    asynStatus status = asynSuccess;

    epicsTimeStamp startTime;

    hid_t dSpace = H5Dget_space(dId);
    if(dSpace < 0)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s couldn't get dataspace\n",
                driverName, functionName);
        return asynError;
    }

    int rank = H5Sget_simple_extent_ndims(dSpace);
    if(rank < 0 || rank != 3)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s couldn't get rank or rank invalid (rank=%d)\n",
                driverName, functionName, rank);
        return asynError;
    }

    hsize_t count[3];
    hsize_t maxdims[3];
    H5Sget_simple_extent_dims(dSpace, count, maxdims);
    count[0] = 1;

    hid_t mSpace = H5Screate_simple(3, count, NULL);

    hsize_t offset[3] = {0,0,0};
    for(offset[0] = 0; offset[0] < nimages; offset[0]++)
    {
        size_t dims[2] = {count[1], count[2]};
        NDDataType_t ndType;
        NDArray *pImage;

        hid_t dTypeId = H5Dget_type(dId);
        if(H5Tequal(dTypeId, H5T_NATIVE_UINT32) > 0)
            ndType = NDUInt32;
        else if(H5Tequal(dTypeId, H5T_NATIVE_UINT16) > 0)
            ndType = NDUInt16;
        else
        {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s:%s invalid data type\n",
                    driverName, functionName);
            status = asynError;
            goto end;
        }

        pImage = pNDArrayPool->alloc(2, dims, ndType, 0, NULL);

        // Select the hyperslab
        if(H5Sselect_hyperslab(dSpace, H5S_SELECT_SET, offset, NULL, count,
                NULL) < 0)
        {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s:%s couldn't select hyperslab\n",
                    driverName, functionName);
            status = asynError;
            pImage->release();
            goto end;
        }

        // the saved datatype in the hdf5 file
        hid_t dType = H5Dget_type(dId);

        // and finally read the image
        if(H5Dread(dId, dType, mSpace, dSpace, H5P_DEFAULT, pImage->pData) < 0)
        {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s:%s couldn't read image\n",
                    driverName, functionName);
            status = asynError;
            pImage->release();
            goto end;
        }

        int imageCounter;
        getIntegerParam(NDArrayCounter, &imageCounter);
        imageCounter++;
        setIntegerParam(NDArrayCounter, imageCounter);
        callParamCallbacks();

        // Put the frame number and time stamp into the buffer
        pImage->uniqueId = imageCounter;
        epicsTimeGetCurrent(&startTime);
        pImage->timeStamp =  startTime.secPastEpoch + startTime.nsec / 1.e9;
        updateTimeStamp(&pImage->epicsTS);

        // Get any attributes that have been defined for this driver
        this->getAttributes(pImage->pAttributeList);

        // Call the NDArray callback
        this->unlock();

        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
                "%s:%s: calling NDArray callback\n",
                driverName, functionName);

        doCallbacksGenericPointer(pImage, NDArrayData, 0);
        this->lock();

        pImage->release();

        callParamCallbacks();
    }

end:
    H5Sclose(mSpace);
    return status;
}

/** This function is called periodically read the detector status (temperature,
  * humidity, etc.). It should not be called if we are acquiring data, to avoid
  * polling server when taking data.*/
asynStatus eigerDetector::eigerStatus (void)
{
    int status;
    double temp = 0.0;
    double humid = 0.0;

    // Read temperature and humidity
    status  = getDouble(SSDetStatus, "board_000/th0_temp",     &temp);
    status |= getDouble(SSDetStatus, "board_000/th0_humidity", &humid);

    if(!status)
    {
        setDoubleParam(ADTemperature, temp);
        setDoubleParam(EigerThTemp0,  temp);
        setDoubleParam(EigerThHumid0, humid);
    }
    else
    {
        setIntegerParam(ADStatus, ADStatusError);
    }

    // Other temperatures/humidities available, do we want them?
    // "board_000/th1_temp"   "board_000/th1_humidity"
    // "module_000/temp"      "module_000/humidity"
    // "module_001/temp"      "module_001/humidity"
    // "module_002/temp"      "module_002/humidity"
    // "module_003/temp"      "module_003/humidity"

    callParamCallbacks();
    return (asynStatus)status;
}


extern "C" int eigerDetectorConfig(const char *portName, const char *serverPort,
        int maxBuffers, size_t maxMemory, int priority, int stackSize)
{
    new eigerDetector(portName, serverPort, maxBuffers, maxMemory, priority,
            stackSize);
    return(asynSuccess);
}

/* Code for iocsh registration */
static const iocshArg eigerDetectorConfigArg0 = {"Port name", iocshArgString};
static const iocshArg eigerDetectorConfigArg1 = {"Server port name",
    iocshArgString};
static const iocshArg eigerDetectorConfigArg2 = {"maxBuffers", iocshArgInt};
static const iocshArg eigerDetectorConfigArg3 = {"maxMemory", iocshArgInt};
static const iocshArg eigerDetectorConfigArg4 = {"priority", iocshArgInt};
static const iocshArg eigerDetectorConfigArg5 = {"stackSize", iocshArgInt};
static const iocshArg * const eigerDetectorConfigArgs[] = {
    &eigerDetectorConfigArg0, &eigerDetectorConfigArg1,
    &eigerDetectorConfigArg2, &eigerDetectorConfigArg3,
    &eigerDetectorConfigArg4, &eigerDetectorConfigArg5};

static const iocshFuncDef configeigerDetector = {"eigerDetectorConfig", 6,
    eigerDetectorConfigArgs};

static void configeigerDetectorCallFunc(const iocshArgBuf *args)
{
    eigerDetectorConfig(args[0].sval, args[1].sval, args[2].ival, args[3].ival,
            args[4].ival, args[5].ival);
}

static void eigerDetectorRegister(void)
{
    iocshRegister(&configeigerDetector, configeigerDetectorCallFunc);
}

extern "C" {
    epicsExportRegistrar(eigerDetectorRegister);
}
