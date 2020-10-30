#pragma once

#include <base.h>
//#include <libpldm/bios.h>
//#include <libpldm/fru.h>
#include <platform.h>

#include <err.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <fstream>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <utility>
#pragma once

#include "sensor.hpp"

#include <boost/asio/deadline_timer.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>

#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <stdint.h>
#include <stdlib.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>

enum pldm_numeric_sensor_commands{
	SetNumericSensorEnable = 0x1,
	GetSensorReading = 0x2,
	GetSensorThresholds = 0x3,
	SetSensorThresholds = 0x4,
	RestoreSensorThresholds = 0x5,
	GetSensorHysteresis = 0x6,
	SetSensorHysteresis = 0x7,
	InitNumericSensor = 0x8
};

struct PLDMSensor : public Sensor
{
    PLDMSensor(std::shared_ptr<sdbusplus::asio::connection>& conn,
                  boost::asio::io_service& io, const std::string& name,
                  const std::string& sensorConfiguration,
                  sdbusplus::asio::object_server& objectServer,
                  std::vector<thresholds::Threshold>&& thresholds,
                  uint16_t sensorId, const std::string& sensorTypeName, 
                  const std::string& sensorUnit, double factor, int sensorScale,
                  const std::string& objectType);
    ~PLDMSensor();

    void checkThresholds(void) override;
    void sensor_read_loop(void);
    void init(void);

    std::pair<int, std::vector<uint8_t>> createGetSensorReadingRequestMsg(uint16_t sensorId, bool8_t rearmEventState);
    std::pair<int, std::vector<uint8_t>> createSetNumericSensorEnableRequestMsg(uint16_t sensorId, uint8_t sensor_operational_state, uint8_t sensor_event_message_enable);
    std::pair<int, std::vector<uint8_t>> createSetSensorThresholdRequestMsg(uint16_t sensorId, uint8_t sensorDataSize, uint32_t THRESHOLDs_val[]);
    std::pair<int, std::vector<uint8_t>> createGetSensorThresholdRequestMsg(uint16_t sensorId);
    void check_init_status(void);

    uint8_t instance_id;
    uint16_t sensorId;
    double sensorFactor;
    const std::string sensorName;
    std::string powerState;
    uint8_t sensorDataSize;
    uint32_t THRESHOLDs_val_sensor[6];
    bool8_t rearmEventState;

    volatile pldm_numeric_sensor_commands cmd;
    volatile pldm_numeric_sensor_commands last_cmd;

  private:
    sdbusplus::asio::object_server& objectServer;
    std::shared_ptr<sdbusplus::asio::connection> dbusConnection;

    boost::asio::deadline_timer waitTimer;
	//boost::asio::deadline_timer initTimer;
	//std::unique_ptr<boost::asio::deadline_timer> waitTimer;
	//std::unique_ptr<boost::asio::deadline_timer> initTimer;
};

/** @brief Print the buffer
 *
 *  @param[in]  buffer  - Buffer to print
 *  @param[in]  pldmVerbose -verbosity flag - true/false
 *
 *  @return - None
 */
void printBuffer(const std::vector<uint8_t>& buffer, bool pldmVerbose);

/** @brief print the input message if pldmverbose is enabled
 *
 *  @param[in]  pldmVerbose - verbosity flag - true/false
 *  @param[in]  msg         - message to print
 *  @param[in]  data        - data to print
 *
 *  @return - None
 */

template <class T>
void Logger(bool pldmverbose, const char* msg, const T& data)
{
    if (pldmverbose)
    {
        std::stringstream s;
        s << data;
        std::cout << msg << s.str() << std::endl;
    }
}

int parseGetThresholdResponseMsg(pldm_msg* responsePtr, size_t payloadLength, int THRESHOLDs_val[] );
int parseSetThresholdResponseMsg(struct pldm_msg* responsePtr, size_t payloadLength);

int parseGetTidResponseMsg(pldm_msg* responsePtr, size_t payloadLength, uint8_t *tid);
int parseSetTidResponseMsg(pldm_msg* responsePtr, size_t payloadLength);

int parseSensorReadingResponseMsg(pldm_msg* responsePtr, size_t payloadLength, double *PRESENT_val);

int parseGetTypeResponseMsg(pldm_msg* responsePtr, size_t payloadLength, pldm_supported_types pldmType);

int parseSetNumericSensorEnableResponseMsg(pldm_msg* responsePtr, size_t payloadLength);

std::pair<int, std::vector<uint8_t>> createSetTIDRequestMsg(uint8_t TID);
std::pair<int, std::vector<uint8_t>> createGetTIDRequestMsg();
std::pair<int, std::vector<uint8_t>> createGetTypeRequestMsg();

int convertDBusToJSON(const std::string& returnType,
                      sdbusplus::message::message& m, nlohmann::json& response);

constexpr uint8_t PLDM_ENTITY_ID = 8;
constexpr uint8_t MCTP_MSG_TYPE_PLDM = 1;

const std::map<const char*, pldm_supported_types> pldmTypes{
    {"base", PLDM_BASE},   {"platform", PLDM_PLATFORM},
    {"bios", PLDM_BIOS},   {"fru", PLDM_FRU},
};

enum pldm_device_state {
	SET_TID = 0x1,
	GET_TID = 0x2,
	GET_TYPE = 0x3,
	OPER_SENSORS = 0x4
};

