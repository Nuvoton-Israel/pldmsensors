/*
// Copyright (c) 2019 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/
#include <err.h>
#include <getopt.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>
#include <limits>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "PLDMSensor.hpp"
#include "Utils.hpp"
#include "VariantVisitors.hpp"

#include <math.h>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/container/flat_map.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>

#include <chrono>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <vector>
#include <map>

using namespace std;

namespace fs = std::filesystem;
using Json = nlohmann::json;

bool debugP = false;

static constexpr double PLDMMaxReading = 0xFF;
static constexpr double PLDMMinReading = 0x0;
uint8_t instance_id_g = 0;
boost::mutex mutex_instanceID;
boost::container::flat_map<std::string, std::unique_ptr<PLDMSensor>> sensors;

volatile pldm_device_state pldm_state = SET_TID;
volatile pldm_device_state last_pldm_state;

boost::asio::io_service io;
std::unique_ptr<boost::asio::deadline_timer> pldmdeviceTimer;
auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);


constexpr auto MAPPER_BUSNAME = "xyz.openbmc_project.ObjectMapper";
constexpr auto MAPPER_PATH = "/xyz/openbmc_project/object_mapper";
constexpr auto MAPPER_INTERFACE = "xyz.openbmc_project.ObjectMapper";

constexpr auto PROPERTY_INTERFACE = "org.freedesktop.DBus.Properties";

map<uint8_t, string> instanceIDmap;
static constexpr const char* sensorPathPrefix = "/xyz/openbmc_project/sensors/";
const std::string& sensorConfiguration="/xyz/openbmc_project/inventory/system/chassis";

PLDMSensor::PLDMSensor(std::shared_ptr<sdbusplus::asio::connection>& conn,
                             boost::asio::io_service& io,
                             const std::string& sensorName,
                             const std::string& sensorConfiguration,
                             sdbusplus::asio::object_server& objectServer,
                             std::vector<thresholds::Threshold>&& thresholdData,
                             uint16_t sensorId, const std::string& sensorTypeName,
                             const std::string& sensorUnit,
                             double factor, int sensorScale,
                             const std::string& objectType) :
    Sensor(boost::replace_all_copy(sensorName, " ", "_"),
           std::move(thresholdData),
           sensorConfiguration,
           objectType, PLDMMaxReading,
           PLDMMinReading, conn),
    sensorId(sensorId), sensorFactor(factor),
    objectServer(objectServer), dbusConnection(conn), waitTimer(io), sensorName(sensorName)
{
    std::string dbusPath = sensorPathPrefix + sensorTypeName + "/" + name;
    sensorInterface = objectServer.add_interface(dbusPath, "xyz.openbmc_project.Sensor.Value");

    std::string UnitPath = "xyz.openbmc_project.Sensor.Value.Unit." + sensorUnit;
    sensorInterface->register_property("Unit", UnitPath );
    sensorInterface->register_property("Scale", sensorScale);

    if (thresholds::hasWarningInterface(thresholds))
    {
        thresholdInterfaceWarning = objectServer.add_interface(
            dbusPath,
            "xyz.openbmc_project.Sensor.Threshold.Warning");
    }
    if (thresholds::hasCriticalInterface(thresholds))
    {
        thresholdInterfaceCritical = objectServer.add_interface(
            dbusPath,
            "xyz.openbmc_project.Sensor.Threshold.Critical");
    }

    association = objectServer.add_interface(
        dbusPath,
        association::interface);
}

PLDMSensor::~PLDMSensor()
{
    waitTimer.cancel();
    objectServer.remove_interface(thresholdInterfaceWarning);
    objectServer.remove_interface(thresholdInterfaceCritical);
    objectServer.remove_interface(sensorInterface);
    objectServer.remove_interface(association);
}

void PLDMSensor::check_init_status(void)
{
    waitTimer.expires_from_now(boost::posix_time::seconds(5));
    waitTimer.async_wait([&](const boost::system::error_code& ec) {
        fprintf(stderr,"%s: check_init_status() time's up: cmd(%d),last_cmd(%d)\n",sensorName.c_str(), cmd, last_cmd);
        if (ec == boost::asio::error::operation_aborted)
        {
            fprintf(stderr,"%s: we're being cancelled in check_init_status\n",sensorName.c_str());
            return; // we're being cancelled
        }
        // read timer error
        else if (ec)
        {
            fprintf(stderr,"%s: timer error check_init_status\n",sensorName.c_str());
            return;
        }

        if( cmd == last_cmd )
        {
            //remove iter in InstanceID map
            map<uint8_t, string>::iterator iter;
            iter = instanceIDmap.find(instance_id);
            if(iter != instanceIDmap.end())
            {
                auto sensorName_m = iter->second ;
                if(sensorName_m.compare(sensorName)==0)
                {
                    instanceIDmap.erase(iter);
                    if(debugP)
                        fprintf(stderr,"%s: find instanceID %d in map, so erase it\n",sensorName.c_str(),instance_id);
                }
            }
            init();
        }
        if( cmd != GetSensorReading )
        {
            fprintf(stderr,"%s: cmd(%d) is still not GetSensorReading, so keep check init status\n",sensorName.c_str(),cmd);
            check_init_status();
        }
        else
            fprintf(stderr,"%s: cmd(%d) is  GetSensorReading, so do not need to check init status\n",sensorName.c_str(),cmd);

    });

}
void PLDMSensor::init(void)
{
        mutex_instanceID.lock();
        instance_id = instance_id_g++;
        if(debugP)
            fprintf(stderr,"%s: In init::instance_id :%x sensorId:%x sensorDataSize:%x\n",sensorName.c_str(),instance_id,sensorId,sensorDataSize);

        map<uint8_t, string>::iterator iter;
        iter = instanceIDmap.find(instance_id);
        if(iter != instanceIDmap.end())
        {
            auto sensorName_m = iter->second ;
            instanceIDmap.erase(iter);
            if(debugP)
                fprintf(stderr,"%s: erase instance ID:%d for reusing in Init \n",sensorName_m.c_str(),instance_id);
        }

        instanceIDmap.insert(pair<uint8_t, string>(instance_id, sensorName));

        if(instance_id_g>0x1F)
            instance_id_g=0;
        mutex_instanceID.unlock();

        if( cmd == SetNumericSensorEnable )
        {
            if(debugP)
                fprintf(stderr,"%s: In init::SetNumericSensorEnable sensorId:%d instance_id:%d\n",sensorName.c_str(),sensorId,instance_id);
            uint8_t dstEid = 8;
            uint8_t msgTag = 1;
            uint8_t sensor_operational_state = 0x0;
            uint8_t sensor_event_message_enable = 0x0;
            auto [rc, requestMsg] = createSetNumericSensorEnableRequestMsg(sensorId, sensor_operational_state, sensor_event_message_enable);
            if (rc != PLDM_SUCCESS)
            {
                std::cerr << "Failed to encode request message for SetNumericSensorEnable" << " rc = " << rc << "\n";
                return;
            }

            requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);

            auto method = dbusConnection->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                              "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
            method.append(dstEid, msgTag, true, requestMsg);
            dbusConnection->call_noreply(method);
            last_cmd = SetNumericSensorEnable;
            return;
        }
        else if( cmd == SetSensorThresholds )
        {
            if(debugP)
                fprintf(stderr,"%s: In init::SetSensorThresholds sensorDataSize:%x\n",sensorName.c_str(),sensorDataSize);
            uint8_t dstEid = 8;
            uint8_t msgTag = 1;
            auto [rc, requestMsg] = createSetSensorThresholdRequestMsg(sensorId, sensorDataSize, THRESHOLDs_val_sensor);
            if (rc != PLDM_SUCCESS)
            {
                std::cerr << "Failed to encode request message for SetSensorThresholds" << " rc = " << rc << "\n";
                return;
            }

            requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);

            auto method = dbusConnection->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                              "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
            method.append(dstEid, msgTag, true, requestMsg);
            dbusConnection->call_noreply(method);
            last_cmd = SetSensorThresholds;
            return;
        }
        else if( cmd == GetSensorThresholds)
        {
            if(debugP)
                fprintf(stderr,"%s: In init::GetSensorThresholds\n",sensorName.c_str());
            uint8_t dstEid = 8;
            uint8_t msgTag = 1;

            auto [rc, requestMsg] = createGetSensorThresholdRequestMsg(sensorId);
            if (rc != PLDM_SUCCESS)
            {
                std::cerr << "Failed to encode request message for SetSensorThresholds" << " rc = " << rc << "\n";
                return;
            }

            requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);

            auto method = dbusConnection->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                              "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
            method.append(dstEid, msgTag, true, requestMsg);
            dbusConnection->call_noreply(method);
            last_cmd = GetSensorThresholds;
            return;
        }//sensor->hysteresis
        else if( cmd == SetSensorHysteresis )
        {
            if(debugP)
                fprintf(stderr,"%s: In init::SetSensorHysteresis: sensorDataSize:%x hysteresis:%d\n",sensorName.c_str(),sensorDataSize, hysteresis);
            uint8_t dstEid = 8;
            uint8_t msgTag = 1;
            auto [rc, requestMsg] = createSetSensorHysteresisRequestMsg(sensorId, sensorDataSize, hysteresis);
            if (rc != PLDM_SUCCESS)
            {
                std::cerr << "Failed to encode request message for SetSensorHysteresis" << " rc = " << rc << "\n";
                return;
            }

            requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);

            auto method = dbusConnection->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                              "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
            method.append(dstEid, msgTag, true, requestMsg);
            dbusConnection->call_noreply(method);
            last_cmd = SetSensorHysteresis;
            return;
        }
        else if( cmd == GetSensorHysteresis)
        {
            if(debugP)
                fprintf(stderr,"%s: In init::GetSensorHysteresis\n",sensorName.c_str());
            uint8_t dstEid = 8;
            uint8_t msgTag = 1;

            auto [rc, requestMsg] = createGetSensorHysteresisRequestMsg(sensorId);
            if (rc != PLDM_SUCCESS)
            {
                std::cerr << "Failed to encode request message for SetSensorThresholds" << " rc = " << rc << "\n";
                return;
            }

            requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);

            auto method = dbusConnection->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                              "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
            method.append(dstEid, msgTag, true, requestMsg);
            dbusConnection->call_noreply(method);
            last_cmd = GetSensorHysteresis;
            return;
        }
        else
            fprintf(stderr,"%s: In init::unexpected cmd:%d\n",sensorName.c_str(),cmd);

}

void PLDMSensor::checkThresholds(void)
{
    thresholds::checkThresholds(this);
}

std::string getService(sdbusplus::bus::bus& bus, std::string path,
                       std::string interface)
{
    auto mapper = bus.new_method_call(MAPPER_BUSNAME, MAPPER_PATH,
                                      MAPPER_INTERFACE, "GetObject");

    mapper.append(path, std::vector<std::string>({interface}));

    std::map<std::string, std::vector<std::string>> mapperResponse;

    auto mapperResponseMsg = bus.call(mapper);

    mapperResponseMsg.read(mapperResponse);
    if (mapperResponse.empty())
    {
        fprintf(stderr,"Error reading mapper response PATH=%s INTERFACE=%s", path.c_str(), interface.c_str());
        throw std::runtime_error("Error reading mapper response");
    }

    return mapperResponse.begin()->first;
}

std::string getProperty(sdbusplus::bus::bus& bus, std::string path,
                        std::string interface, std::string propertyName)
{
    std::variant<std::string> property;
    std::string service = getService(bus, path, interface);

    auto method = bus.new_method_call(service.c_str(), path.c_str(),
                                      PROPERTY_INTERFACE, "Get");

    method.append(interface, propertyName);
    auto reply = bus.call(method);
    reply.read(property);

    if (std::get<std::string>(property).empty())
    {
        fprintf(stderr,"Error reading property response %s\n", propertyName.c_str());
        throw std::runtime_error("Error reading property response");
    }

    return std::get<std::string>(property);
}

void PLDMSensor::sensor_read_loop(void)
{
    static constexpr size_t pollTime = 1; // in seconds

    waitTimer.expires_from_now(boost::posix_time::seconds(pollTime));
    waitTimer.async_wait([this](const boost::system::error_code& ec) {

        if(debugP)
            fprintf(stderr,"%s: PLDMSensor::read : sensorId =%X\n",sensorName.c_str(),sensorId);

        if (ec == boost::asio::error::operation_aborted)
        {
            fprintf(stderr,"%s: we're being cancelled sensor_read_loop\n",sensorName.c_str());
            return; // we're being cancelled
        }
        // read timer error
        else if (ec)
        {
            fprintf(stderr,"%s: timer error in sensor_read_loop\n",sensorName.c_str());
            return;
        }

        std::string hostPath = "/xyz/openbmc_project/state/host0";
        auto bus = sdbusplus::bus::new_default();
        auto OSState =
            getProperty(bus, hostPath, "xyz.openbmc_project.State.OperatingSystem.Status", "OperatingSystemState");
        std::string workingState = "xyz.openbmc_project.State.OperatingSystem.Status.OSStatus." + powerState;

        if( OSState.compare(workingState)==0 )//xyz.openbmc_project.State.OperatingSystem.Status.OSStatus.Standby
        {
            mutex_instanceID.lock();
            instance_id = instance_id_g++;

            map<uint8_t, string>::iterator iter;

            iter = instanceIDmap.find(instance_id);
            if(iter != instanceIDmap.end())
            {
                auto sensorName_m = iter->second ;
                instanceIDmap.erase(iter);
                if(debugP)
                    fprintf(stderr,"%s: erase instance ID:%d for reusing in sensor_reading\n",sensorName_m.c_str(),instance_id);

            }

            instanceIDmap.insert(pair<uint8_t, string>(instance_id, sensorName));

            if(instance_id_g>0x1F)
                instance_id_g=0;
            mutex_instanceID.unlock();

            if( cmd == GetSensorReading )
            {
                uint8_t dstEid = 8;
                uint8_t msgTag = 1;
                auto [rc, requestMsg] = createGetSensorReadingRequestMsg(sensorId, rearmEventState);
                if (rc != PLDM_SUCCESS)
                {
                    std::cerr << "Failed to encode request message for sensor reading" << " rc = " << rc << "\n";
                    return;
                }
                requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);
                auto method = dbusConnection->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                                  "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
                method.append(dstEid, msgTag, true, requestMsg);
                dbusConnection->call_noreply(method);
            }
        }
        sensor_read_loop();
    });
}

void createSensors(
    boost::asio::io_service& io, sdbusplus::asio::object_server& objectServer,
    boost::container::flat_map<std::string, std::unique_ptr<PLDMSensor>>&
        sensors,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection)
{
    if (!dbusConnection)
    {
        std::cerr << "Connection not created\n";
        return;
    }
    const std::string pldmsensors_config = "/etc/default/pldmsensors.json";
    std::ifstream jsonFile(pldmsensors_config);
    if (!jsonFile.is_open())
    {
        printf("pldmsensor config file does not exist, FILE=%s\n",pldmsensors_config.c_str());
        return;
    }

    auto data = Json::parse(jsonFile, nullptr, false);
    if (data.is_discarded())
    {
        printf("Parsing config file failed\n");
        return;
    }

    jsonFile.close();

    for (const auto& record : data)
    {
        uint16_t sensorId;
        int sensorScale;
        std::string factor_s;
        std::string sensorTypeName;
        std::string sensorName;
        std::string sensorUnit;
        std::string objectType;
        std::string powerState;
        std::string THRESHOLDs_val_s[6];
        double factor;
        int THRESHOLDs_val[6];
        std::string sensorDataSize;
        std::string rearmEventState;
        int hysteresis_int;

        constexpr auto sensorID_json = "sensorID";
        constexpr auto sensorScale_json = "sensorScale";
        constexpr auto factor_json = "factor";
        constexpr auto sensorTypeName_json = "sensorTypeName";
        constexpr auto sensorName_json = "sensorName";
        constexpr auto sensorUnit_json = "sensorUnit";
        constexpr auto objectType_json = "objectType";
        constexpr auto upperThresholdWarning_json = "upperThresholdWarning";
        constexpr auto upperThresholdCritical_json = "upperThresholdCritical";
        constexpr auto upperThresholdFatal_json = "upperThresholdFatal";
        constexpr auto lowerThresholdWarning_json = "lowerThresholdWarning";
        constexpr auto lowerThresholdCritical_json = "lowerThresholdCritical";
        constexpr auto lowerThresholdFatal_json = "lowerThresholdFatal";
        constexpr auto sensorDataSize_json = "sensorDataSize";
        constexpr auto rearmEventState_json = "rearmEventState";
        constexpr auto powerState_json = "powerState";
        constexpr auto hysteresis_json = "hysteresis";

        sensorId = static_cast<uint16_t>(record.value(sensorID_json, 0));
        sensorScale = record.value(sensorScale_json, 0);
        factor_s = record.value(factor_json, "");
        sensorTypeName = record.value(sensorTypeName_json, "");
        sensorName = record.value(sensorName_json, "");
        sensorUnit = record.value(sensorUnit_json, "");
        sensorDataSize = record.value(sensorDataSize_json, "");
        objectType = record.value(objectType_json, "");
        powerState = record.value(powerState_json, "");
        hysteresis_int = record.value(hysteresis_json, 0);
        rearmEventState = record.value(rearmEventState_json, "");
        THRESHOLDs_val_s[0] = record.value(upperThresholdWarning_json, "");
        THRESHOLDs_val_s[1] = record.value(upperThresholdCritical_json, "");
        THRESHOLDs_val_s[2] = record.value(upperThresholdFatal_json, "");
        THRESHOLDs_val_s[3] = record.value(lowerThresholdWarning_json, "");
        THRESHOLDs_val_s[4] = record.value(lowerThresholdCritical_json, "");
        THRESHOLDs_val_s[5] = record.value(lowerThresholdFatal_json, "");

        factor = atof(factor_s.c_str());
        for(int i=0 ; i<6 ; i++)
            THRESHOLDs_val[i] = atoi(THRESHOLDs_val_s[i].c_str());

        std::vector<thresholds::Threshold> sensorThresholds;

        thresholds::Level level;
        thresholds::Direction direction;
//WARNING
        level = thresholds::Level::WARNING;

        direction = thresholds::Direction::HIGH;
        sensorThresholds.emplace_back(level, direction, THRESHOLDs_val[0]*factor);

        direction = thresholds::Direction::LOW;
        sensorThresholds.emplace_back(level, direction, THRESHOLDs_val[3]*factor);

//CRITICAL
        level = thresholds::Level::CRITICAL;

        direction = thresholds::Direction::HIGH;
        sensorThresholds.emplace_back(level, direction, THRESHOLDs_val[1]*factor);

        direction = thresholds::Direction::LOW;
        sensorThresholds.emplace_back(level, direction, THRESHOLDs_val[4]*factor);

        auto& sensor = sensors[sensorName];

        sensor = std::make_unique<PLDMSensor>(
        dbusConnection, io, sensorName, sensorConfiguration, objectServer,
        std::move(sensorThresholds),
        sensorId, sensorTypeName, sensorUnit,
        factor, sensorScale,objectType);
        sensor->powerState = powerState;
        sensor->hysteresis = hysteresis_int;

        for(int i=0 ; i<6 ; i++)
            sensor->THRESHOLDs_val_sensor[i] = THRESHOLDs_val[i];

        sensor->cmd = SetNumericSensorEnable;
        if( sensorDataSize.compare("UINT8") == 0)
            sensor->sensorDataSize = PLDM_EFFECTER_DATA_SIZE_UINT8;
        else if( sensorDataSize.compare("SINT8") == 0)
            sensor->sensorDataSize = PLDM_EFFECTER_DATA_SIZE_SINT8;
        else if( sensorDataSize.compare("UINT16") == 0)
            sensor->sensorDataSize = PLDM_EFFECTER_DATA_SIZE_UINT16;
        else if( sensorDataSize.compare("SINT16") == 0)
            sensor->sensorDataSize = PLDM_EFFECTER_DATA_SIZE_SINT16;
        else if( sensorDataSize.compare("UINT32") == 0)
            sensor->sensorDataSize = PLDM_EFFECTER_DATA_SIZE_UINT32;
        else if( sensorDataSize.compare("SINT32") == 0)
            sensor->sensorDataSize = PLDM_EFFECTER_DATA_SIZE_SINT32;
        else
            sensor->sensorDataSize = -1;

        if( rearmEventState.compare("true") == 0)
            sensor->rearmEventState = 0x1;
        else
            sensor->rearmEventState = 0x0;
        sensor->setInitialProperties(dbusConnection);
        sensor->init();
        sensor->check_init_status();
    }
    return;
}

void check_pldm_device_status(void)
{
    uint8_t TID = 2;
    uint8_t dstEid = 8;
    uint8_t msgTag = 1;

    pldmdeviceTimer->expires_from_now(boost::posix_time::seconds(3));
    // create a timer because normally multiple properties change
    pldmdeviceTimer->async_wait([&](const boost::system::error_code& ec) {
        fprintf(stderr,"check_pldm_device_status() time's up (pldm_state:%d)(last_pldm_state:%d)\n",pldm_state, last_pldm_state);
        if (ec == boost::asio::error::operation_aborted)
        {
            std::cerr << "we're being cancelled\n";
            return; // we're being cancelled
        }
        // read timer error
        else if (ec)
        {
            std::cerr << "timer error\n";
            return;
        }

        if( (pldm_state!=OPER_SENSORS) && (last_pldm_state == pldm_state) )
        {
            fprintf(stderr,"check_pldm_device_status() : time's up(last_pldm_state:%d)==(pldm_state:%d)\n",last_pldm_state,pldm_state);
            map<uint8_t, string>::iterator iter;
            iter = instanceIDmap.find(0);
            if(iter != instanceIDmap.end())
            {
                auto sensorName_m = iter->second ;
                if(sensorName_m.compare("root")==0)
                {
                    instanceIDmap.erase(iter);
                    fprintf(stderr,"In check_pldm_device_status, instanceID 0 in map is belonging to root, so erase it\n");
                }
            }
            if( last_pldm_state == SET_TID)
            {
                fprintf(stderr,"resend SET_TID command\n");
                auto [rc, requestMsg] = createSetTIDRequestMsg(TID);
                if (rc != PLDM_SUCCESS)
                {
                    std::cerr << "Failed to encode request message for SetTID" << " rc = " << rc << "\n";
                    return;
                }
                requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);

                auto method = systemBus->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                                        "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
                method.append(dstEid, msgTag, true, requestMsg);

                systemBus->call_noreply(method);
                instanceIDmap.insert(pair<uint8_t, string>(0, "root"));
            }
            else if( last_pldm_state == GET_TID)
            {
                fprintf(stderr,"resend GET_TID command\n");
                auto [rc, requestMsg] = createGetTIDRequestMsg();
                if (rc != PLDM_SUCCESS)
                {
                    std::cerr << "Failed to encode request message for SetTID" << " rc = " << rc << "\n";
                    return;
                }
                requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);

                auto method = systemBus->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                                  "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
                method.append(dstEid, msgTag, true, requestMsg);

                systemBus->call_noreply(method);
                instanceIDmap.insert(pair<uint8_t, string>(0, "root"));
            }
            else if( last_pldm_state == GET_TYPE)
            {
                fprintf(stderr,"resend GET_TYPE command\n");
                auto [rc, requestMsg] = createGetTypeRequestMsg();
                if (rc != PLDM_SUCCESS)
                {
                    std::cerr << "Failed to encode request message for GetTypes" << " rc = " << rc << "\n";
                    return;
                }
                requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);

                auto method = systemBus->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                                  "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
                method.append(dstEid, msgTag, true, requestMsg);

                systemBus->call_noreply(method);
                instanceIDmap.insert(pair<uint8_t, string>(0, "root"));
            }
            else
                pldmdeviceTimer->cancel();
        }

        if( pldm_state < OPER_SENSORS )
            check_pldm_device_status();
        else
            pldmdeviceTimer->cancel();
    });
}

int main(void)
{
    last_pldm_state = pldm_state = SET_TID;

    systemBus->request_name("xyz.openbmc_project.PLDMSensor");//Create Service
    sdbusplus::asio::object_server objectServer(systemBus);
    std::vector<std::unique_ptr<sdbusplus::bus::match::match>> matches;

    uint8_t TID = 2;
    uint8_t dstEid = 8;
    uint8_t msgTag = 1;

    std::function<void(sdbusplus::message::message&)> eventHandler =
        [&](sdbusplus::message::message& message) {

                if (strcmp(message.get_member(), "MessageReceivedSignal") != 0)
                {
                    fprintf(stderr,"member:%s is unexpected\n",message.get_member());
                    return;
                }

                nlohmann::json data;
                int r = convertDBusToJSON("yyybay", message, data);
                if (r < 0)
                {
                    fprintf(stderr,"convertDBusToJSON failed with %d\n", r);
                    return;
                }
                if (!data.is_array())
                {
                    fprintf(stderr,"No data in MessageReceivedSignal signal\n");
                    return;
                }
                if(debugP)
                    fprintf(stderr,"Got response from MessageReceivedSignal(pldm_state=%d)\n",pldm_state);
                std::vector<uint8_t> responseMsg = data[4].get<std::vector<uint8_t>>();
                auto responsePtr =
                    reinterpret_cast<struct pldm_msg*>(&responseMsg[1]);
                auto resphdr =
                    reinterpret_cast<const pldm_msg_hdr*>(&responseMsg[1]);

                map<uint8_t, string>::iterator iter;

                iter = instanceIDmap.find(resphdr->instance_id);
                if(iter != instanceIDmap.end())
                {
                    auto instance_id = iter->first;
                    auto sensorName = iter->second ;
                    instanceIDmap.erase(iter);
                    if(debugP)
                        fprintf(stderr,"erase InstanceID:%d sensor name:%s due to get resp\n",instance_id,sensorName.c_str());
                    if(pldm_state == SET_TID)
                    {
                        if(debugP)
                            fprintf(stderr,"resp pldm_state = SET_TID\n");
                        if(parseSetTidResponseMsg(responsePtr, responseMsg.size() - sizeof(pldm_msg_hdr) - 1)<0)
                        {
                            fprintf(stderr,"Set TID fail\n");
                            return;
                        }
                        else
                        {
                            pldm_state = GET_TID;
                            fprintf(stderr,"Set TID successfully\n");
                        }

                        auto [rc, requestMsg] = createGetTIDRequestMsg();
                        if (rc != PLDM_SUCCESS)
                        {
                            std::cerr << "Failed to encode request message for SetTID" << " rc = " << rc << "\n";
                            return;
                        }
                        requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);

                        auto method = systemBus->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                                          "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");

                        uint8_t dstEid = 8;
                        uint8_t msgTag = 1;
                        method.append(dstEid, msgTag, true, requestMsg);
                        systemBus->call_noreply(method);
                        instanceIDmap.insert(pair<uint8_t, string>(0, "root"));
                        last_pldm_state = GET_TID;
                        return;
                    }
                    else if(pldm_state == GET_TID)
                    {
                        uint8_t tid;
                        parseGetTidResponseMsg(responsePtr, responseMsg.size() - sizeof(pldm_msg_hdr) - 1, &tid);
                        if( tid != TID )
                        {
                            fprintf(stderr,"TID compare fail\n");
                            return;
                        }

                        fprintf(stderr,"TID compare successfully, then send GetType Req\n");
                        pldm_state = GET_TYPE;

                        auto [rc, requestMsg] = createGetTypeRequestMsg();
                        if (rc != PLDM_SUCCESS)
                        {
                            std::cerr << "Failed to encode request message for GetTypes" << " rc = " << rc << "\n";
                            return;
                        }
                        requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);

                        auto method = systemBus->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                                          "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");

                        uint8_t dstEid = 8;
                        uint8_t msgTag = 1;
                        method.append(dstEid, msgTag, true, requestMsg);
                        systemBus->call_noreply(method);
                        instanceIDmap.insert(pair<uint8_t, string>(0, "root"));
                        last_pldm_state = GET_TYPE;
                        return;
                    }
                    else if (pldm_state == GET_TYPE)
                    {
                        pldm_supported_types pldmType = PLDM_PLATFORM;
                        int r = parseGetTypeResponseMsg(responsePtr, responseMsg.size() - sizeof(pldm_msg_hdr) - 1, pldmType);
                        if(r<0)
                        {
                            fprintf(stderr,"GET_TYPE fail \n");
                            return;
                        }
                        else if(r==0)
                        {
                            fprintf(stderr,"Do not support PLDM_PLATFORM\n");
                            return;
                        }
                        else
                            fprintf(stderr,"This PLDM device supports PLDM_PLATFORM\n");

                        createSensors(io, objectServer, sensors, systemBus);
                        if (sensors.empty())
                        {
                            std::cout << "Configuration not detected\n";
                        }
                        else
                        {
                            fprintf(stderr,"Create Sensors successfully\n");
                            last_pldm_state = pldm_state = OPER_SENSORS;
                        }
                        return;
                    }
                    else if (pldm_state == OPER_SENSORS)
                    {
                        auto& sensor = sensors[sensorName];
                        if(debugP)
                            fprintf(stderr,"%s: Resp instance_id:%x sensor->cmd:%d\n",sensor->sensorName.c_str(),instance_id,sensor->cmd);
                        if(sensor->cmd == GetSensorReading)
                        {
                            double PRESENT_val;
                            if(parseSensorReadingResponseMsg(responsePtr, responseMsg.size() - sizeof(pldm_msg_hdr) - 1, &PRESENT_val)<0)
                            {
                                fprintf(stderr,"%s: Parse SensorReading Response Fail\n",sensor->sensorName.c_str());
                                return;
                            }
                            PRESENT_val = PRESENT_val*sensor->sensorFactor;
                            sensor->updateValue(PRESENT_val);
                        }
                        else if(sensor->cmd == SetNumericSensorEnable)
                        {
                            if(parseSetNumericSensorEnableResponseMsg(responsePtr, responseMsg.size() - sizeof(pldm_msg_hdr) - 1)<0)
                                fprintf(stderr,"%s: Parse SetNumericSensorEnable Response Fail\n",sensor->sensorName.c_str());
                            else
                                sensor->cmd = SetSensorThresholds;
                            sensor->init();
                        }
                        else if(sensor->cmd == SetSensorThresholds)
                        {
                            if(parseSetThresholdResponseMsg(responsePtr, responseMsg.size() - sizeof(pldm_msg_hdr) - 1)<0)
                                fprintf(stderr,"%s: Parse SetSensorThresholds Response Fail\n",sensor->sensorName.c_str());
                            else
                            {
                                sensor->cmd = GetSensorThresholds;
                                fprintf(stderr,"%s: SetSensorThresholds successfully\n",sensor->sensorName.c_str());
                            }
                            sensor->init();
                        }
                        else if(sensor->cmd == GetSensorThresholds)
                        {
                           int THRESHOLDs_val[6];
                           parseGetThresholdResponseMsg(responsePtr, responseMsg.size() - sizeof(pldm_msg_hdr) - 1, THRESHOLDs_val );

                            for(int i=0 ; i<6 ; i++)
                                if(sensor->THRESHOLDs_val_sensor[i] != THRESHOLDs_val[i])
                                {
                                   fprintf(stderr,"%s: Compare sensor threshold fail\n",sensor->sensorName.c_str());
                                   sensor->init();
                                   return;
                                }
                            fprintf(stderr,"%s: Compare sensor threshold successfully, and then SetSensorHysteresis\n",sensor->sensorName.c_str());
                            sensor->cmd = SetSensorHysteresis;
                            sensor->init();
                        }
                        else if(sensor->cmd == SetSensorHysteresis)
                        {
                            fprintf(stderr,"%s: Response of SetSensorHysteresis\n",sensor->sensorName.c_str());
                            if(parseSetSensorHysteresisResponseMsg(responsePtr, responseMsg.size() - sizeof(pldm_msg_hdr) - 1)<0)
                                fprintf(stderr,"%s: Parse SetSensorHysteresis Response Fail\n",sensor->sensorName.c_str());
                            else
                            {
                                fprintf(stderr,"%s: SetSensorHysteresis successfully\n",sensor->sensorName.c_str());
                                sensor->cmd = GetSensorHysteresis;
                            }
                            sensor->init();
                        }
                       else if(sensor->cmd == GetSensorHysteresis)
                       {
                           fprintf(stderr,"%s: Response of GetSensorHysteresis\n",sensor->sensorName.c_str());
                           int Hysteresis_val;
                           parseGetSensorHysteresisResponseMsg(responsePtr, responseMsg.size() - sizeof(pldm_msg_hdr) - 1, &Hysteresis_val );
                           fprintf(stderr,"%s: Hysteresis_val:%d\n",sensorName.c_str(),Hysteresis_val);
                            if(sensor->hysteresis != Hysteresis_val)
                            {
                                fprintf(stderr,"%s: Compare sensor hysteresis fail\n",sensor->sensorName.c_str());
                                sensor->init();
                                return;
                            }

                           sensor->cmd = GetSensorReading;
                           fprintf(stderr,"%s: GetSensorHysteresis successfully, Start sensor read : sensorId =%X\n",sensor->sensorName.c_str(),sensor->sensorId);
                           sensor->sensor_read_loop();
                       }
                        return;
                    }
                }
                else
                    return;
        };

    auto match = std::make_unique<sdbusplus::bus::match::match>(
        static_cast<sdbusplus::bus::bus&>(*systemBus),
        "interface='xyz.openbmc_project.MCTP.Base',type='signal',"
        "member='MessageReceivedSignal',path='/xyz/openbmc_project/mctp'",
        eventHandler);

    matches.emplace_back(std::move(match));

    auto [rc, requestMsg] = createSetTIDRequestMsg(TID);

    if (rc != PLDM_SUCCESS)
    {
        std::cerr << "Failed to encode request message for SetTID" << " rc = " << rc << "\n";
        return 1;
    }
    requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);

    auto method = systemBus->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                      "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
    method.append(dstEid, msgTag, true, requestMsg);
    systemBus->call_noreply(method);

    instanceIDmap.insert(pair<uint8_t, string>(0, "root"));

    pldmdeviceTimer = std::make_unique<boost::asio::deadline_timer>(io);

    check_pldm_device_status();

    io.run();

    return 0;
}

