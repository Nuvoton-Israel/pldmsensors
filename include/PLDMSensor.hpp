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
#include "effecter.hpp"

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

constexpr auto MAPPER_BUSNAME = "xyz.openbmc_project.ObjectMapper";
constexpr auto MAPPER_PATH = "/xyz/openbmc_project/object_mapper";
constexpr auto MAPPER_INTERFACE = "xyz.openbmc_project.ObjectMapper";
constexpr auto PROPERTY_INTERFACE = "org.freedesktop.DBus.Properties";

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

enum pldm_state_sensor_commands{
	SetStateSensorEnable = 0x1,
	GetStateSensorReading = 0x2,
};
/*Effecter--Start*/
struct PLDMEffecter : public Effecter
{
    PLDMEffecter(std::shared_ptr<sdbusplus::asio::connection>& conn,
                  boost::asio::io_service& io,
                  const std::string& name,
                  sdbusplus::asio::object_server& objectServer,
                  uint16_t effecterId,
                  const std::string& effecterTypeName,
                  const std::string& effecterUnit);
    ~PLDMEffecter();

    void init(void);

    uint8_t instance_id;
    uint16_t effecterId;
    const std::string effecterName;
    const std::string effecterTypeName;
    std::string effecterstate1;
    std::string effecterstate2;
    std::string powerState;
    uint8_t effecterDataSize;
    std::string dbusPath;
  private:
    sdbusplus::asio::object_server& objectServer;
    std::shared_ptr<sdbusplus::asio::connection> dbusConnection;
    std::vector<std::unique_ptr<sdbusplus::bus::match::match>> matches;

};

struct PLDMNumericEffecter : public Effecter
{
    PLDMNumericEffecter(std::shared_ptr<sdbusplus::asio::connection>& conn,
                  boost::asio::io_service& io,
                  const std::string& name,
                  sdbusplus::asio::object_server& objectServer,
                  uint16_t effecterId,
                  const std::string& effecterTypeName,
                  const std::string& effecterUnit);
    ~PLDMNumericEffecter();

    void init(void);

    uint8_t instance_id;
    uint16_t effecterId;
    const std::string effecterName;
    const std::string effecterTypeName;

    std::string powerState;
    uint8_t effecterDataSize;
    std::string dbusPath;
  private:
    sdbusplus::asio::object_server& objectServer;
    std::shared_ptr<sdbusplus::asio::connection> dbusConnection;
    std::vector<std::unique_ptr<sdbusplus::bus::match::match>> matches;

};
/*Effecter--End*/

struct PLDMStateSensor : public StateSensor
{
    PLDMStateSensor(std::shared_ptr<sdbusplus::asio::connection>& conn,
                  boost::asio::io_service& io,
                  const std::string& name,
                  sdbusplus::asio::object_server& objectServer,
                  uint16_t sensorId,
                  const std::string& sensorTypeName,
                  const std::string& sensorUnit);
    ~PLDMStateSensor();

    void state_sensor_read_loop(void);
    void init(void);
    void check_init_status(void);

    std::pair<int, std::vector<uint8_t>> createGetStateSensorReadingRequestMsg(uint16_t sensorId, bitfield8_t rearmEventState);
    std::pair<int, std::vector<uint8_t>> createSetStateSensorEnableRequestMsg(uint16_t sensorId, uint8_t sensorOperationalState, uint8_t sensorEventMessageEnable);

    uint8_t instance_id;
    uint16_t sensorId;
    const std::string sensorName;
    const std::string sensorTypeName;
    std::string powerState;
    bitfield8_t rearmEventState;
    std::string dbusPath;

    volatile pldm_state_sensor_commands cmd;
    volatile pldm_state_sensor_commands last_cmd;

  private:
    sdbusplus::asio::object_server& objectServer;
    std::shared_ptr<sdbusplus::asio::connection> dbusConnection;
    boost::asio::deadline_timer waitTimer;

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
    void check_init_status(void);

    std::pair<int, std::vector<uint8_t>> createGetSensorReadingRequestMsg(uint16_t sensorId, bool8_t rearmEventState);
    std::pair<int, std::vector<uint8_t>> createSetNumericSensorEnableRequestMsg(uint16_t sensorId, uint8_t sensor_operational_state, uint8_t sensor_event_message_enable);
    std::pair<int, std::vector<uint8_t>> createSetSensorThresholdRequestMsg(uint16_t sensorId, uint8_t sensorDataSize, uint32_t THRESHOLDs_val[]);
    std::pair<int, std::vector<uint8_t>> createGetSensorThresholdRequestMsg(uint16_t sensorId);
    std::pair<int, std::vector<uint8_t>> createSetSensorHysteresisRequestMsg(uint16_t sensorId, uint8_t sensorDataSize, uint32_t Hysteresis_val);
    std::pair<int, std::vector<uint8_t>> createGetSensorHysteresisRequestMsg(uint16_t sensorId);

    uint8_t instance_id;
    uint16_t sensorId;
    double sensorFactor;
    const std::string sensorName;
    const std::string sensorTypeName;
    std::string powerState;
    uint8_t sensorDataSize;
    uint32_t THRESHOLDs_val_sensor[6];
    bool8_t rearmEventState;
    uint32_t hysteresis;

    volatile pldm_numeric_sensor_commands cmd;
    volatile pldm_numeric_sensor_commands last_cmd;

  private:
    sdbusplus::asio::object_server& objectServer;
    std::shared_ptr<sdbusplus::asio::connection> dbusConnection;

    boost::asio::deadline_timer waitTimer;
};

int parseSetStateSensorEnableResponseMsg(pldm_msg* responsePtr, size_t payloadLength);
int parseStateSensorReadingResponseMsg(pldm_msg* responsePtr, size_t payloadLength, uint8_t retcomp_sensorCnt, get_sensor_state_field *retstateField);

int parseGetSensorHysteresisResponseMsg(pldm_msg* responsePtr, size_t payloadLength, int *hysteresis_Val );
int parseSetSensorHysteresisResponseMsg(pldm_msg* responsePtr, size_t payloadLength);

int parseGetThresholdResponseMsg(pldm_msg* responsePtr, size_t payloadLength, int THRESHOLDs_val[] );
int parseSetThresholdResponseMsg(pldm_msg* responsePtr, size_t payloadLength);

int parseSensorReadingResponseMsg(pldm_msg* responsePtr, size_t payloadLength, double *PRESENT_val);
int parseSetNumericSensorEnableResponseMsg(pldm_msg* responsePtr, size_t payloadLength);

int parseGetTidResponseMsg(pldm_msg* responsePtr, size_t payloadLength, uint8_t *tid);
int parseSetTidResponseMsg(pldm_msg* responsePtr, size_t payloadLength);
int parseGetTypeResponseMsg(pldm_msg* responsePtr, size_t payloadLength, pldm_supported_types pldmType);

//State Effecter
int parseSetEffecterStateResponseMsg(pldm_msg* responsePtr, size_t payloadLength);
int parseGetEffecterStateResponseMsg(pldm_msg* responsePtr, size_t payloadLength, uint8_t retcomp_sensorCnt, get_sensor_state_field *retstateField);

//Numeric Effecter
int parseGetNumericEffecterResponseMsg(pldm_msg* responsePtr, size_t payloadLength,
        uint8_t *reteffecter_dataSize, uint8_t *reteffecter_operState, uint8_t *retpendingValue, uint8_t *retpresentValue);
int parseSetNumericEffecterResponseMsg(pldm_msg* responsePtr, size_t payloadLength);

std::pair<int, std::vector<uint8_t>> createSetTIDRequestMsg(uint8_t TID);
std::pair<int, std::vector<uint8_t>> createGetTIDRequestMsg();
std::pair<int, std::vector<uint8_t>> createGetTypeRequestMsg();

int convertDBusToJSON(const std::string& returnType,
                      sdbusplus::message::message& m, nlohmann::json& response);

std::string getProperty(sdbusplus::bus::bus& bus, std::string path,
                        std::string interface, std::string propertyName);

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
