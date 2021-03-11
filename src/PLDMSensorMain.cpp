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


extern "C" {
#include <errno.h>
#include <i2c/smbus.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
}


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
boost::container::flat_map<std::string, std::unique_ptr<PLDMEffecter>> effecters;
boost::container::flat_map<std::string, std::unique_ptr<PLDMStateSensor>> state_sensors;
boost::container::flat_map<std::string, std::unique_ptr<PLDMNumericEffecter>> numeric_effecters;

volatile pldm_device_state pldm_state = SET_TID;
volatile pldm_device_state last_pldm_state;

boost::asio::io_service io;
std::unique_ptr<boost::asio::deadline_timer> pldmdeviceTimer;
auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);

map<uint8_t, string> instanceIDmap;
const std::string& numericsensorPathPrefix = "/xyz/openbmc_project/numericsensors/";
const std::string& statesensorPathPrefix = "/xyz/openbmc_project/statesensors/";

const std::string& numericeffecterPathPrefix = "/xyz/openbmc_project/numericeffecters/";
const std::string& stateeffecterPathPrefix = "/xyz/openbmc_project/stateeffecters/";


const std::string& sensorConfiguration="/xyz/openbmc_project/inventory/system/chassis";

/*Effecter--Start*/
//State-Effecter

PLDMEffecter::PLDMEffecter(std::shared_ptr<sdbusplus::asio::connection>& conn,
                             boost::asio::io_service& io,
                             const std::string& effecterName,
                             sdbusplus::asio::object_server& objectServer,
                             uint16_t effecterId,
                             const std::string& effecterTypeName,
                             const std::string& effecterUnit
                             ) :
    Effecter(boost::replace_all_copy(effecterName, " ", "_"),
           conn),
    effecterId(effecterId),
    objectServer(objectServer), dbusConnection(conn),
    effecterName(effecterName), effecterTypeName(effecterTypeName)
{
    dbusPath = stateeffecterPathPrefix + effecterTypeName + "/" + name;

    effecterInterface = objectServer.add_interface(dbusPath, effecterStateInterface);
    effecterInterface->register_property("effecterId", effecterId);

    effecterInterface->register_property("effecterOperationalState",std::numeric_limits<uint8_t>::quiet_NaN());
    effecterInterface->register_property("present_state",std::numeric_limits<uint8_t>::quiet_NaN());
    effecterInterface->register_property("previous_state",std::numeric_limits<uint8_t>::quiet_NaN());
    effecterInterface->register_property("event_state",std::numeric_limits<uint8_t>::quiet_NaN());
    effecterInterface->register_property("completeionCode",std::numeric_limits<uint8_t>::quiet_NaN());
    effecterInterface->register_property("setDeviceState",std::numeric_limits<string>::quiet_NaN(), sdbusplus::asio::PropertyPermission::readWrite);

    effecterInterface->register_method(
        "SetEffecterStateMessagePayload",
        [this](uint16_t effecter_id, bool set_request, uint8_t effecter_state) {
            uint8_t dstEid = 8;
            uint8_t msgTag = 1;
            uint8_t instance_id = 0;
            uint8_t compEffecterCnt = 0x1;

            fprintf(stderr,"SetEffecterStateMessagePayload: name:%s\n",name.c_str());
            //Get instanceID for PLDM msg--start
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

            instanceIDmap.insert(pair<uint8_t, string>(instance_id, name));

            if(instance_id_g>0x1F)
                instance_id_g=0;
            mutex_instanceID.unlock();
            //Get instanceID for PLDM msg--End

            std::array<set_effecter_state_field, 8> stateField{};

            if(set_request)
                stateField[0] = {PLDM_REQUEST_SET, effecter_state};
            else
                stateField[0] = {PLDM_NO_CHANGE, effecter_state};

            std::vector<uint8_t> requestMsg(sizeof(pldm_msg_hdr) + 5);

            auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
            auto rc = encode_set_state_effecter_states_req(
                instance_id, effecter_id, compEffecterCnt, stateField.data(), request);

            if (rc != PLDM_SUCCESS)
            {
                std::cerr << "Failed to encode request message for sensor reading" << " rc = " << rc << "\n";
                return -1;
            }
            requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);
            auto method = systemBus->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                              "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
            method.append(dstEid, msgTag, true, requestMsg);
            systemBus->call_noreply(method);
            return 0;
    });

    effecterInterface->register_method(
        "GetEffecterStateMessagePayload",
        [this](uint16_t effecter_id) {
            uint8_t dstEid = 8;
            uint8_t msgTag = 1;
            uint8_t instance_id = 0;
            uint8_t compEffecterCnt = 0x1;

            fprintf(stderr,"GetEffecterStateMessagePayload: name:%s\n",name.c_str());
            //Get instanceID for PLDM msg--start
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

            instanceIDmap.insert(pair<uint8_t, string>(instance_id, name));

            if(instance_id_g>0x1F)
                instance_id_g=0;
            mutex_instanceID.unlock();
            //Get instanceID for PLDM msg--End

            std::vector<uint8_t> requestMsg(sizeof(pldm_msg_hdr) + 2);
            auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
            auto rc = encode_get_state_effecter_states_req( instance_id, effecter_id, request);

            if (rc != PLDM_SUCCESS)
            {
                std::cerr << "Failed to encode request message for sensor reading" << " rc = " << rc << "\n";
                return -1;
            }
            requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);
            auto method = systemBus->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                              "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
            method.append(dstEid, msgTag, true, requestMsg);
            systemBus->call_noreply(method);
            return 0;
    });
}

PLDMEffecter::~PLDMEffecter()
{
    objectServer.remove_interface(effecterInterface);
}

void PLDMEffecter::init(void)
{
    std::function<void(sdbusplus::message::message&)> eventHandlerProperty =
        [&](sdbusplus::message::message& message) {

                if (strcmp(message.get_member(), "PropertiesChanged") != 0)
                {
                    fprintf(stderr,"member:%s is unexpected\n",message.get_member());
                    return;
                }


                using Interface = std::string;
                using Property = std::string;
                using Value = std::string;
                using Properties = std::map<Property, std::variant<Value>>;

                Interface interface;
                Properties properties;

                message.read(interface, properties);

                for (const auto& p : properties)
                {
                    auto property_s = p.first;
                    if(property_s.compare("setDeviceState")==0)
                    {
                        auto value_s = std::get<std::string>(p.second);
                        fprintf(stderr,"PropertiesChanged for [%s][%s] \n", name.c_str(), message.get_path());
                        fprintf(stderr,"    interface is %s\n",interface.c_str());
                        fprintf(stderr,"        Property:%s Value:%s \n",p.first.c_str(),value_s.c_str());

                        std::array<set_effecter_state_field, 8> stateField{};
                        if(value_s.compare(effecterstate1)==0)
                        {
                            fprintf(stderr,"set to state1\n");
                            uint8_t effecter_state = 0x1;
                            stateField[0] = {PLDM_REQUEST_SET, effecter_state};
                        }
                        else if(value_s.compare(effecterstate2)==0)
                        {
                            fprintf(stderr,"set to state 2\n");
                            uint8_t effecter_state = 0x2;
                            stateField[0] = {PLDM_REQUEST_SET, effecter_state};
                        }

                        uint8_t dstEid = 8;
                        uint8_t msgTag = 1;
                        uint8_t instance_id = 0;
                        uint8_t compEffecterCnt = 0x1;


                        //Get instanceID for PLDM msg--start
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

                        instanceIDmap.insert(pair<uint8_t, string>(instance_id, name));

                        if(instance_id_g>0x1F)
                            instance_id_g=0;
                        mutex_instanceID.unlock();
                        //Get instanceID for PLDM msg--End


                        std::vector<uint8_t> requestMsg(sizeof(pldm_msg_hdr) + 5);
                        fprintf(stderr,"encode a set state effecter state request\n");
                        auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
                        auto rc = encode_set_state_effecter_states_req(
                            instance_id, effecterId, compEffecterCnt, stateField.data(), request);

                        if (rc != PLDM_SUCCESS)
                        {
                            std::cerr << "Failed to encode request message for sensor reading" << " rc = " << rc << "\n";
                            return ;
                        }
                        requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);
                        auto method = systemBus->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                                          "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
                        method.append(dstEid, msgTag, true, requestMsg);
                        systemBus->call_noreply(method);
                        fprintf(stderr,"send a set state effecter state request\n");
                    }
                }

        };

    auto matchPropertry = std::make_unique<sdbusplus::bus::match::match>(
        static_cast<sdbusplus::bus::bus&>(*systemBus),
        "interface='org.freedesktop.DBus.Properties',type='signal',"
        "member='PropertiesChanged',path='"+ dbusPath + "'",
        eventHandlerProperty);

    matches.emplace_back(std::move(matchPropertry));

/*Get state efecter state first time*/
    fprintf(stderr,"get state_effecter state in init()\n");
    uint8_t dstEid = 8;
    uint8_t msgTag = 1;

    mutex_instanceID.lock();
    instance_id = instance_id_g++;

    map<uint8_t, string>::iterator iter;

    iter = instanceIDmap.find(instance_id);
    if(iter != instanceIDmap.end())
    {
        auto effecterName_m = iter->second ;
        instanceIDmap.erase(iter);
        if(debugP)
            fprintf(stderr,"%s: erase instance ID:%d for reusing in effecter init\n",effecterName_m.c_str(),instance_id);
    }

    instanceIDmap.insert(pair<uint8_t, string>(instance_id, effecterName));

    if(instance_id_g>0x1F)
        instance_id_g=0;
    mutex_instanceID.unlock();
    //Get instanceID for PLDM msg--End

    std::vector<uint8_t> requestMsg(sizeof(pldm_msg_hdr) + 2);
    auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
    auto rc = encode_get_state_effecter_states_req( instance_id, effecterId, request);

    if (rc != PLDM_SUCCESS)
    {
        std::cerr << "Failed to encode request message for state effect reading" << " rc = " << rc << "\n";
        return;
    }
    requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);
    auto method = systemBus->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                      "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
    method.append(dstEid, msgTag, true, requestMsg);
    systemBus->call_noreply(method);
}
//State-Effecter
//Numeric-Effecter

PLDMNumericEffecter::PLDMNumericEffecter(std::shared_ptr<sdbusplus::asio::connection>& conn,
                             boost::asio::io_service& io,
                             const std::string& effecterName,
                             sdbusplus::asio::object_server& objectServer,
                             uint16_t effecterId,
                             const std::string& effecterTypeName,
                             const std::string& effecterUnit
                             ) :
    Effecter(boost::replace_all_copy(effecterName, " ", "_"),
           conn),
    effecterId(effecterId),
    objectServer(objectServer), dbusConnection(conn),
    effecterName(effecterName), effecterTypeName(effecterTypeName)
{
    uint8_t effecterOperationalState = 0;
    uint32_t pendingValue = 0;
    uint32_t presentValue = 0;
    std::string setNumericValue = "nan";
    dbusPath = numericeffecterPathPrefix + effecterTypeName + "/" + name;

    effecterInterface = objectServer.add_interface(dbusPath, effecterStateInterface);
    effecterInterface->register_property("effecterId", effecterId);

    effecterInterface->register_property("effecterOperationalState",effecterOperationalState);
    effecterInterface->register_property("pendingValue",pendingValue);
    effecterInterface->register_property("presentValue",presentValue);
    effecterInterface->register_property("setNumericValue",setNumericValue, sdbusplus::asio::PropertyPermission::readWrite);
    effecterInterface->register_property("effecterUnit",effecterUnit);
    effecterInterface->register_method(
        "SetEffecterNumericMessagePayload",
        [this](uint16_t effecter_id, std::string effecter_value_s) {
            uint8_t dstEid = 8;
            uint8_t msgTag = 1;
            uint8_t instance_id = 0;
            uint8_t compEffecterCnt = 0x1;

            fprintf(stderr,"SetEffecterNumericMessagePayload: name:%s effecter_value_s.size():%d\n",name.c_str(),effecter_value_s.size());
            //Get instanceID for PLDM msg--start
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

            instanceIDmap.insert(pair<uint8_t, string>(instance_id, name));

            if(instance_id_g>0x1F)
                instance_id_g=0;
            mutex_instanceID.unlock();
            //Get instanceID for PLDM msg--End

            uint32_t value = atoi(effecter_value_s.c_str());
            uint8_t effecter_value[4] = {};
            uint8_t size;
            if (effecterDataSize == PLDM_EFFECTER_DATA_SIZE_UINT8 ||
               effecterDataSize == PLDM_EFFECTER_DATA_SIZE_SINT8)
            {
                size = 0;
                effecter_value[0] = value;
            }
           else if (effecterDataSize == PLDM_EFFECTER_DATA_SIZE_UINT16 ||
               effecterDataSize == PLDM_EFFECTER_DATA_SIZE_SINT16)
            {
                size = 1;
                value = htole16(value);
                memcpy(effecter_value, &value, 2);
            }
           else if (effecterDataSize == PLDM_EFFECTER_DATA_SIZE_UINT32 ||
               effecterDataSize == PLDM_EFFECTER_DATA_SIZE_SINT32)
            {
                size = 3;
                value = htole32(value);
                memcpy(effecter_value, &value, 4);
            }

            std::vector<uint8_t> requestMsg(sizeof(pldm_msg_hdr) + PLDM_SET_NUMERIC_EFFECTER_VALUE_MIN_REQ_BYTES + size);

            auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());

            auto rc = encode_set_numeric_effecter_value_req(
                instance_id, effecter_id, size,
                effecter_value,
                request,
                PLDM_SET_NUMERIC_EFFECTER_VALUE_MIN_REQ_BYTES + size);

            if (rc != PLDM_SUCCESS)
            {
                std::cerr << "Failed to encode request message for sensor reading" << " rc = " << rc << "\n";
                return -1;
            }
            requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);
            auto method = systemBus->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                              "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
            method.append(dstEid, msgTag, true, requestMsg);
            systemBus->call_noreply(method);
            return 0;
    });

    effecterInterface->register_method(
        "GetEffecterNumericMessagePayload",
        [this](uint16_t effecter_id) {
            uint8_t dstEid = 8;
            uint8_t msgTag = 1;
            uint8_t instance_id = 0;

            fprintf(stderr,"GetEffecterNumericMessagePayload: name:%s\n",name.c_str());
            //Get instanceID for PLDM msg--start
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

            instanceIDmap.insert(pair<uint8_t, string>(instance_id, name));

            if(instance_id_g>0x1F)
                instance_id_g=0;
            mutex_instanceID.unlock();
            //Get instanceID for PLDM msg--End

            std::vector<uint8_t> requestMsg(sizeof(pldm_msg_hdr) + PLDM_GET_NUMERIC_EFFECTER_VALUE_REQ_BYTES);
            auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
            auto rc = encode_get_numeric_effecter_value_req(instance_id, effecter_id, request);

            if (rc != PLDM_SUCCESS)
            {
                std::cerr << "Failed to encode request message for sensor reading" << " rc = " << rc << "\n";
                return -1;
            }
            requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);
            auto method = systemBus->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                              "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
            method.append(dstEid, msgTag, true, requestMsg);
            systemBus->call_noreply(method);
            return 0;
    });
}

PLDMNumericEffecter::~PLDMNumericEffecter()
{
    objectServer.remove_interface(effecterInterface);
}

void PLDMNumericEffecter::init(void)
{
    std::function<void(sdbusplus::message::message&)> eventHandlerProperty =
        [&](sdbusplus::message::message& message) {

                if (strcmp(message.get_member(), "PropertiesChanged") != 0)
                {
                    fprintf(stderr,"member:%s is unexpected\n",message.get_member());
                    return;
                }

                using Interface = std::string;
                using Property = std::string;
                using Value = std::string;
                using Properties = std::map<Property, std::variant<Value>>;

                Interface interface;
                Properties properties;

                message.read(interface, properties);

                for (const auto& p : properties)
                {
                    auto property_s = p.first;
                    if(property_s.compare("setNumericValue")==0)
                    {
                        auto value_s = std::get<std::string>(p.second);
                        fprintf(stderr,"PropertiesChanged for NumericEffecter [%s][%s] \n", name.c_str(), message.get_path());
                        fprintf(stderr,"    interface:%s\n",interface.c_str());
                        fprintf(stderr,"    Property:%s Value:%s \n",p.first.c_str(),value_s.c_str());

                        uint8_t dstEid = 8;
                        uint8_t msgTag = 1;
                        uint8_t instance_id = 0;

                        //Get instanceID for PLDM msg--start
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

                        instanceIDmap.insert(pair<uint8_t, string>(instance_id, name));

                        if(instance_id_g>0x1F)
                            instance_id_g=0;
                        mutex_instanceID.unlock();
                        //Get instanceID for PLDM msg--End

                        uint32_t value = atoi(value_s.c_str());
                        uint8_t effecter_value[4] = {};
                        uint8_t size;
                        if (effecterDataSize == PLDM_EFFECTER_DATA_SIZE_UINT8 ||
                           effecterDataSize == PLDM_EFFECTER_DATA_SIZE_SINT8)
                        {
                            fprintf(stderr,"effecterDataSize is INT8\n");
                            size = 0;
                            effecter_value[0] = value;
                        }
                       else if (effecterDataSize == PLDM_EFFECTER_DATA_SIZE_UINT16 ||
                           effecterDataSize == PLDM_EFFECTER_DATA_SIZE_SINT16)
                        {
                            fprintf(stderr,"effecterDataSize is INT16\n");
                            size = 1;
                            value = htole16(value);
                            memcpy(effecter_value, &value, 2);
                        }
                       else if (effecterDataSize == PLDM_EFFECTER_DATA_SIZE_UINT32 ||
                           effecterDataSize == PLDM_EFFECTER_DATA_SIZE_SINT32)
                        {
                            fprintf(stderr,"effecterDataSize is INT32\n");
                            size = 3;
                            value = htole32(value);
                            memcpy(effecter_value, &value, 4);
                        }

                        std::vector<uint8_t> requestMsg(sizeof(pldm_msg_hdr) + PLDM_SET_NUMERIC_EFFECTER_VALUE_MIN_REQ_BYTES + size);

                        auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
                        fprintf(stderr,"encode a set numeric effecter value request\n");
                        auto rc = encode_set_numeric_effecter_value_req(
                            instance_id, effecterId, size,
                            effecter_value,
                            request,
                            PLDM_SET_NUMERIC_EFFECTER_VALUE_MIN_REQ_BYTES + size);


                        if (rc != PLDM_SUCCESS)
                        {
                            std::cerr << "Failed to encode request message for sensor reading" << " rc = " << rc << "\n";
                            return ;
                        }
                        requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);
                        auto method = systemBus->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                                          "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
                        method.append(dstEid, msgTag, true, requestMsg);
                        systemBus->call_noreply(method);
                        fprintf(stderr,"send a set numeric effecter value request\n");
                    }
                }

        };

    auto matchPropertry = std::make_unique<sdbusplus::bus::match::match>(
        static_cast<sdbusplus::bus::bus&>(*systemBus),
        "interface='org.freedesktop.DBus.Properties',type='signal',"
        "member='PropertiesChanged',path='"+ dbusPath + "'",
        eventHandlerProperty);

    matches.emplace_back(std::move(matchPropertry));

    fprintf(stderr,"Send get_numeric_effecter_value req in init() [%s]\n", name.c_str());
    uint8_t dstEid = 8;
    uint8_t msgTag = 1;

    mutex_instanceID.lock();
    instance_id = instance_id_g++;

    map<uint8_t, string>::iterator iter;

    iter = instanceIDmap.find(instance_id);
    if(iter != instanceIDmap.end())
    {
        auto effecterName_m = iter->second ;
        instanceIDmap.erase(iter);
        if(debugP)
            fprintf(stderr,"%s: erase instance ID:%d for reusing in effecter init\n",effecterName_m.c_str(),instance_id);
    }

    instanceIDmap.insert(pair<uint8_t, string>(instance_id, effecterName));

    if(instance_id_g>0x1F)
        instance_id_g=0;
    mutex_instanceID.unlock();
    //Get instanceID for PLDM msg--End

    std::vector<uint8_t> requestMsg(sizeof(pldm_msg_hdr) + 2);
    auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
    auto rc = encode_get_numeric_effecter_value_req( instance_id, effecterId, request);

    if (rc != PLDM_SUCCESS)
    {
        std::cerr << "Failed to encode request message for state effect reading" << " rc = " << rc << "\n";
        return;
    }
    requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);
    auto method = systemBus->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                      "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
    method.append(dstEid, msgTag, true, requestMsg);
    systemBus->call_noreply(method);
}

/*Effecter--End*/

PLDMStateSensor::PLDMStateSensor(std::shared_ptr<sdbusplus::asio::connection>& conn,
                             boost::asio::io_service& io,
                             const std::string& sensorName,
                             sdbusplus::asio::object_server& objectServer,
                             uint16_t sensorId,
                             const std::string& sensorTypeName,
                             const std::string& sensorUnit
                             ) :
    StateSensor(boost::replace_all_copy(sensorName, " ", "_"),
           conn),
    sensorId(sensorId),
    objectServer(objectServer), dbusConnection(conn), waitTimer(io),
    sensorName(sensorName), sensorTypeName(sensorTypeName)
{
    dbusPath = statesensorPathPrefix + sensorTypeName + "/" + name;

    stateSensorInterface = objectServer.add_interface(dbusPath, sensorStateInterface);
    stateSensorInterface->register_property("sensorId", sensorId);

    stateSensorInterface->register_property("sensorOperationalState",std::numeric_limits<uint8_t>::quiet_NaN());
    stateSensorInterface->register_property("present_state",std::numeric_limits<uint8_t>::quiet_NaN());
    stateSensorInterface->register_property("previous_state",std::numeric_limits<uint8_t>::quiet_NaN());
    stateSensorInterface->register_property("event_state",std::numeric_limits<uint8_t>::quiet_NaN());

}

PLDMStateSensor::~PLDMStateSensor()
{
    objectServer.remove_interface(stateSensorInterface);
}

void PLDMStateSensor::check_init_status(void)
{
    waitTimer.expires_from_now(boost::posix_time::seconds(5));
    waitTimer.async_wait([&](const boost::system::error_code& ec) {
        fprintf(stderr,"PLDMStateSensor::%s: check_init_status() time's up: cmd(%d),last_cmd(%d)\n",sensorName.c_str(), cmd, last_cmd);
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
        if( cmd != GetStateSensorReading )
        {
            fprintf(stderr,"PLDMStateSensor::%s: cmd(%d) is still not GetStateSensorReading, so keep check init status\n",sensorName.c_str(),cmd);
            check_init_status();
        }
        else
            fprintf(stderr,"PLDMStateSensor::%s: cmd(%d) is  GetStateSensorReading, so do not need to check init status\n",sensorName.c_str(),cmd);

    });
}

void PLDMStateSensor::init(void)
{
    mutex_instanceID.lock();
    instance_id = instance_id_g++;
    if(debugP)
        fprintf(stderr,"init():: %s : instance_id :%x sensorId:%x\n",sensorName.c_str(),instance_id,sensorId);

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

    if( cmd == SetStateSensorEnable )
    {
        if(debugP)
            fprintf(stderr,"%s: In init::SetStateSensorEnable sensorId:%d instance_id:%d\n",sensorName.c_str(),sensorId,instance_id);
        uint8_t dstEid = 8;
        uint8_t msgTag = 1;

        uint8_t sensorOperationalState = PLDM_SENSOR_ENABLED;
        uint8_t sensorEventMessageEnable = PLDM_ENABLE_EVENTS;

        auto [rc, requestMsg] = createSetStateSensorEnableRequestMsg(sensorId, sensorOperationalState, sensorEventMessageEnable);

        if (rc != PLDM_SUCCESS)
        {
            std::cerr << "Failed to encode request message for SetStateSensorEnable" << " rc = " << rc << "\n";
            return;
        }

        requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);

        auto method = dbusConnection->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                          "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
        method.append(dstEid, msgTag, true, requestMsg);
        dbusConnection->call_noreply(method);
        last_cmd = SetStateSensorEnable;
        return;
    }
    else
        fprintf(stderr,"%s: In init::unexpected cmd:%d\n",sensorName.c_str(),cmd);

}

void PLDMStateSensor::state_sensor_read_loop(void)
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

            if( cmd == GetStateSensorReading )
            {
                uint8_t dstEid = 8;
                uint8_t msgTag = 1;
                auto [rc, requestMsg] = createGetStateSensorReadingRequestMsg(sensorId, rearmEventState);
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
        state_sensor_read_loop();
    });
}


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
    objectServer(objectServer), dbusConnection(conn), waitTimer(io),
    sensorName(sensorName), sensorTypeName(sensorTypeName)
{
    std::string dbusPath = numericsensorPathPrefix + sensorTypeName + "/" + name;
    sensorInterface = objectServer.add_interface(dbusPath, sensorValueInterface);

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
        fprintf(stderr,"PLDMSensor::%s: check_init_status() time's up: cmd(%d),last_cmd(%d)\n",sensorName.c_str(), cmd, last_cmd);
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
            fprintf(stderr,"PLDMSensor::%s: cmd(%d) is still not GetSensorReading, so keep check init status\n",sensorName.c_str(),cmd);
            check_init_status();
        }
        else
            fprintf(stderr,"PLDMSensor::%s: cmd(%d) is  GetSensorReading, so do not need to check init status\n",sensorName.c_str(),cmd);

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
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    Json data)
{
    if (!dbusConnection)
    {
        std::cerr << "Connection not created\n";
        return;
    }

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

void createStateSensors(
    boost::asio::io_service& io, sdbusplus::asio::object_server& objectServer,
    boost::container::flat_map<std::string, std::unique_ptr<PLDMStateSensor>>&
        state_sensors,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    Json data)
{
    if (!dbusConnection)
    {
        std::cerr << "Connection not created\n";
        return;
    }

    for (const auto& record : data)
    {
        uint16_t sensorId;
        std::string sensorTypeName;
        std::string sensorName;
        std::string sensorUnit;
        std::string powerState;
        uint8_t rearmEventState;


        constexpr auto sensorID_json = "sensorID";
        constexpr auto sensorTypeName_json = "sensorTypeName";
        constexpr auto sensorName_json = "sensorName";
        constexpr auto sensorUnit_json = "sensorUnit";

        constexpr auto rearmEventState_json = "rearmEventState";
        constexpr auto powerState_json = "powerState";


        sensorId = static_cast<uint16_t>(record.value(sensorID_json, 0));
        sensorTypeName = record.value(sensorTypeName_json, "");
        sensorName = record.value(sensorName_json, "");
        sensorUnit = record.value(sensorUnit_json, "");
        powerState = record.value(powerState_json, "");
        rearmEventState = static_cast<uint8_t>(record.value(rearmEventState_json, 0));

        auto& sensor = state_sensors[sensorName];

        sensor = std::make_unique<PLDMStateSensor>(
        dbusConnection, io, sensorName, objectServer,
        sensorId, sensorTypeName, sensorUnit);

        sensor->powerState = powerState;

        sensor->rearmEventState.byte = rearmEventState;

        sensor->setInitialProperties(dbusConnection);

        sensor->cmd = SetStateSensorEnable;
        sensor->init();
        sensor->check_init_status();
    }
    return;
}

void createEffecters(
    boost::asio::io_service& io, sdbusplus::asio::object_server& objectServer,
    boost::container::flat_map<std::string, std::unique_ptr<PLDMEffecter>>&
        effecters,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    Json data)
{
    if (!dbusConnection)
    {
        std::cerr << "Connection not created\n";
        return;
    }

    for (const auto& record : data)
    {
        uint16_t effecterId;
        std::string effecterTypeName;
        std::string effecterName;
        std::string effecterUnit;
        std::string powerState;
        std::string effecterDataSize;
        std::string effecterState1;
        std::string effecterState2;

        constexpr auto effecterId_json = "effecterID";
        constexpr auto effecterTypeName_json = "effecterTypeName";
        constexpr auto effecterName_json = "effecterName";
        constexpr auto effecterUnit_json = "effecterUnit";
        constexpr auto effecterDataSize_json = "effecterDataSize";
        constexpr auto powerState_json = "powerState";
        constexpr auto effecterState1_json = "effecterstate1";
        constexpr auto effecterState2_json = "effecterstate2";

        effecterId = static_cast<uint16_t>(record.value(effecterId_json, 0));
        effecterTypeName = record.value(effecterTypeName_json, "");
        effecterName = record.value(effecterName_json, "");
        effecterUnit = record.value(effecterUnit_json, "");
        effecterDataSize = record.value(effecterDataSize_json, "");
        effecterState1 = record.value(effecterState1_json, "");
        effecterState2 = record.value(effecterState2_json, "");
        powerState = record.value(powerState_json, "");

        auto& effecter = effecters[effecterName];

        effecter = std::make_unique<PLDMEffecter>(
        dbusConnection, io, effecterName, objectServer,
        effecterId, effecterTypeName, effecterUnit);

        effecter->powerState = powerState;
        effecter->effecterstate1 = effecterState1;
        effecter->effecterstate2 = effecterState2;
        effecter->setInitialProperties(dbusConnection);
        effecter->init();
    }
    return;
}

void createNumericEffecters(
    boost::asio::io_service& io, sdbusplus::asio::object_server& objectServer,
    boost::container::flat_map<std::string, std::unique_ptr<PLDMNumericEffecter>>&
        numeric_effecters,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    Json data)
{
    if (!dbusConnection)
    {
        std::cerr << "Connection not created\n";
        return;
    }

    for (const auto& record : data)
    {
        uint16_t effecterId;
        std::string effecterTypeName;
        std::string effecterName;
        std::string effecterUnit;
        std::string powerState;
        std::string effecterDataSize;

        constexpr auto effecterId_json = "effecterID";
        constexpr auto effecterTypeName_json = "effecterTypeName";
        constexpr auto effecterName_json = "effecterName";
        constexpr auto effecterUnit_json = "effecterUnit";
        constexpr auto effecterDataSize_json = "effecterDataSize";
        constexpr auto powerState_json = "powerState";

        effecterId = static_cast<uint16_t>(record.value(effecterId_json, 0));
        effecterTypeName = record.value(effecterTypeName_json, "");
        effecterName = record.value(effecterName_json, "");
        effecterUnit = record.value(effecterUnit_json, "");
        effecterDataSize = record.value(effecterDataSize_json, "");
        powerState = record.value(powerState_json, "");

        auto& effecter = numeric_effecters[effecterName];

        effecter = std::make_unique<PLDMNumericEffecter>(
        dbusConnection, io, effecterName, objectServer,
        effecterId, effecterTypeName, effecterUnit);

        effecter->setInitialProperties(dbusConnection);

        effecter->powerState = powerState;

        if( effecterDataSize.compare("UINT8") == 0)
            effecter->effecterDataSize = PLDM_EFFECTER_DATA_SIZE_UINT8;
        else if( effecterDataSize.compare("SINT8") == 0)
            effecter->effecterDataSize = PLDM_EFFECTER_DATA_SIZE_SINT8;
        else if( effecterDataSize.compare("UINT16") == 0)
            effecter->effecterDataSize = PLDM_EFFECTER_DATA_SIZE_UINT16;
        else if( effecterDataSize.compare("SINT16") == 0)
            effecter->effecterDataSize = PLDM_EFFECTER_DATA_SIZE_SINT16;
        else if( effecterDataSize.compare("UINT32") == 0)
            effecter->effecterDataSize = PLDM_EFFECTER_DATA_SIZE_UINT32;
        else if( effecterDataSize.compare("SINT32") == 0)
            effecter->effecterDataSize = PLDM_EFFECTER_DATA_SIZE_SINT32;
        else
            effecter->effecterDataSize = -1;

        effecter->init();

    }
    return;
}

void create_sensor_effecter(
    sdbusplus::asio::object_server& objectServer)
{
    const std::string pldm_node_configPath = "/etc/default/pldm.json";
    std::ifstream jsonFile(pldm_node_configPath);
    if (!jsonFile.is_open())
    {
        fprintf(stderr," PLDM config file(%s) doesn't exist!!\n",pldm_node_configPath.c_str());
        return;
    }

    Json jsonConfig = Json::parse(jsonFile, nullptr, false);

    if (jsonConfig.size() == 0 )
    {
        fprintf(stderr," PLDM config file(%s) size is 0!!\n",pldm_node_configPath.c_str());
        return;
    }

    if ( jsonConfig.contains("numeric_sensor") )
    {
        fprintf(stderr,"Find numeric_sensor in pldm.json file\n");
        createSensors(io, objectServer, sensors, systemBus, jsonConfig.at("numeric_sensor"));
            if (sensors.empty())
            {
                std::cout << "numeric_sensor not detected\n";
            }
            else
            {
                fprintf(stderr,"Create numeric_sensor successfully\n");
                last_pldm_state = pldm_state = OPER_SENSORS;
            }
    }
    else
        fprintf(stderr,"No numeric_sensor in pldm.json file\n");

    if ( jsonConfig.contains("state_sensor") )
    {
        fprintf(stderr,"Find state_sensor in pldm.json file\n");
        createStateSensors(io, objectServer, state_sensors, systemBus, jsonConfig.at("state_sensor"));
            if (state_sensors.empty())
            {
                std::cout << "state_sensor not detected\n";
            }
            else
            {
                fprintf(stderr,"Create state_sensor successfully\n");
                last_pldm_state = pldm_state = OPER_SENSORS;
            }
    }
    else
        fprintf(stderr,"No state_sensor in pldm.json file\n");

    if ( jsonConfig.contains("state_effecter") )
    {
        fprintf(stderr,"Find state_effecter in pldm.json file\n");
        createEffecters(io, objectServer, effecters, systemBus, jsonConfig.at("state_effecter"));
        if (effecters.empty())
        {
            std::cout << "state_effecter Configuration not detected\n";
        }
        else
        {
            fprintf(stderr,"Create state_effecter successfully\n");
            last_pldm_state = pldm_state = OPER_SENSORS;
        }
    }
    else
        fprintf(stderr,"No state_effecter in pldm.json file\n");

    if ( jsonConfig.contains("numeric_effecter") )
    {
        fprintf(stderr,"Find numeric_effecter in pldm.json file\n");
        createNumericEffecters(io, objectServer, numeric_effecters, systemBus, jsonConfig.at("numeric_effecter"));
        if (numeric_effecters.empty())
        {
            std::cout << "numeric_effecter Configuration not detected\n";
        }
        else
        {
            fprintf(stderr,"Create numeric_effecter successfully\n");
            last_pldm_state = pldm_state = OPER_SENSORS;
        }
    }
    else
        fprintf(stderr,"No numeric_effecter in pldm.json file\n");

    return;
}

void check_pldm_device_status(void)
{
    pldmdeviceTimer->expires_from_now(boost::posix_time::seconds(5));
    // create a timer because normally multiple properties change
    pldmdeviceTimer->async_wait([&](const boost::system::error_code& ec) {
        uint8_t TID = 2;
        uint8_t dstEid = 8;
        uint8_t msgTag = 1;
        if (ec == boost::asio::error::operation_aborted)
        {
            std::cerr << "check_pldm_device_status(): we're being cancelled\n";
            return; // we're being cancelled
        }
        // read timer error
        else if (ec)
        {
            std::cerr << "check_pldm_device_status(): timer error\n";
            return;
        }

        fprintf(stderr,"check_pldm_device_status() time's up (pldm_state:%d)(last_pldm_state:%d)\n",pldm_state, last_pldm_state);

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
                fprintf(stderr,"resend SET_TID command(dstEid:%d)\n",dstEid);
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
                fprintf(stderr,"resend GET_TID command(dstEid:%d)\n",dstEid);
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
                fprintf(stderr,"resend GET_TYPE command(dstEid:%d)\n",dstEid);
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

                        //Read pldm sensor effecter config file
                        create_sensor_effecter(objectServer);
                        return;
                    }
                    else if (pldm_state == OPER_SENSORS)
                    {
                        /* State Effecter
                         *    PLDM_SET_STATE_EFFECTER_ENABLE = 0x38,
                         *    PLDM_SET_STATE_EFFECTER_STATES = 0x39,
                         *    PLDM_GET_STATE_EFFECTER_STATES = 0x3A,
                         */
                        if( resphdr->command>=PLDM_SET_STATE_EFFECTER_ENABLE && resphdr->command<=PLDM_GET_STATE_EFFECTER_STATES )
                        {
                            auto& effecter = effecters[sensorName];
                            if(!effecter)
                            {
                                fprintf(stderr,"got STATE_EFFECTER Response, but STATE_EFFECTER is not ready\n");
                                return;
                            }
                            if( resphdr->command == PLDM_SET_STATE_EFFECTER_STATES )
                            {
                                if(parseSetEffecterStateResponseMsg(responsePtr, responseMsg.size() - sizeof(pldm_msg_hdr) - 1)<0)
                                {
                                    uint8_t CC = 0x1;
                                    fprintf(stderr,"%s: Parse SetEffecterState Response Fail\n",effecter->effecterName.c_str());
                                    effecter->effecterInterface->set_property("completeionCode", CC);
                                    return;
                                }
                                else
                                {
                                    fprintf(stderr,"Parse Response of SET_STATE_EFFECTER_STATES successfully [%s]\n", effecter->effecterName.c_str());
                                    uint8_t CC = 0x0;
                                    effecter->effecterInterface->set_property("completeionCode", CC);

                                    //Send GetEffecterState--Start
                                    uint8_t dstEid = 8;
                                    uint8_t msgTag = 1;
                                    uint8_t instance_id = 0;
                                    uint8_t compEffecterCnt = 0x1;

                                    //Get instanceID for PLDM msg--start
                                    mutex_instanceID.lock();
                                    instance_id = instance_id_g++;

                                    map<uint8_t, string>::iterator iter;

                                    iter = instanceIDmap.find(instance_id);
                                    if(iter != instanceIDmap.end())
                                    {
                                        auto effecterName_m = iter->second ;
                                        instanceIDmap.erase(iter);
                                        if(debugP)
                                            fprintf(stderr,"%s: erase instance ID:%d for reusing in sensor_reading\n",effecterName_m.c_str(),instance_id);
                                    }

                                    instanceIDmap.insert(pair<uint8_t, string>(instance_id, effecter->effecterName));

                                    if(instance_id_g>0x1F)
                                        instance_id_g=0;
                                    mutex_instanceID.unlock();
                                    //Get instanceID for PLDM msg--End

                                    std::vector<uint8_t> requestMsg(sizeof(pldm_msg_hdr) + 2);
                                    auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
                                    auto rc = encode_get_state_effecter_states_req( instance_id, effecter->effecterId, request);

                                    if (rc != PLDM_SUCCESS)
                                    {
                                        std::cerr << "Failed to encode request message for state effect reading" << " rc = " << rc << "\n";
                                        return;
                                    }
                                    requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);
                                    auto method = systemBus->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                                                      "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
                                    method.append(dstEid, msgTag, true, requestMsg);
                                    systemBus->call_noreply(method);
                                    //Resend GetEffecterState--End
                                }
                            }
                            else if( resphdr->command == PLDM_GET_STATE_EFFECTER_STATES )
                            {
                                uint8_t retcomp_sensorCnt=1;
                                get_sensor_state_field retstateField[8];
                                if(parseGetEffecterStateResponseMsg(responsePtr, responseMsg.size() - sizeof(pldm_msg_hdr) - 1, retcomp_sensorCnt, retstateField)<0)
                                {
                                    uint8_t CC = 0x1;
                                    fprintf(stderr,"%s: Parse GetEffecterState Response Fail\n",effecter->effecterName.c_str());
                                    effecter->effecterInterface->set_property("completeionCode", CC);
                                    return;
                                }
                                else
                                {
                                    fprintf(stderr,"Parse response of GetEffecterState Successfully [%s]\n", effecter->effecterName.c_str());
                                    for(int i=0; i<retcomp_sensorCnt; i++)
                                    {
                                        effecter->effecterInterface->set_property("effecterOperationalState", retstateField[i].sensor_op_state);
                                        effecter->effecterInterface->set_property("present_state", retstateField[i].present_state);
                                        effecter->effecterInterface->set_property("previous_state", retstateField[i].previous_state);
                                        effecter->effecterInterface->set_property("event_state", retstateField[i].event_state);

                                        if(debugP)
                                        {
                                            fprintf(stderr,"    %d:present_state :%x\n",i,retstateField[i].present_state);
                                            fprintf(stderr,"    %d:previous_state:%x\n",i,retstateField[i].previous_state);
                                            fprintf(stderr,"    %d:sensor_op_state:%x\n",i,retstateField[i].sensor_op_state);
                                            fprintf(stderr,"    %d:event_state:%x\n",i,retstateField[i].event_state);
                                        }
                                    }
                                    uint8_t CC = 0x0;
                                    effecter->effecterInterface->set_property("completeionCode", CC);
                                    return;
                                }
                            }
                        }
                        /* Numeric Effecter
                         *    PLDM Numeric Effecter commands
                         *    PLDM_SET_NUMERIC_EFFECTER_ENABLE = 0x30,
                         *    PLDM_SET_NUMERIC_EFFECTER_VALUE = 0x31,
                         *    PLDM_GET_NUMERIC_EFFECTER_VALUE = 0x32,
                         */
                        else if( resphdr->command>=PLDM_SET_NUMERIC_EFFECTER_ENABLE && resphdr->command<=PLDM_GET_NUMERIC_EFFECTER_VALUE )
                        {
                            auto& effecter = numeric_effecters[sensorName];
                            if(!effecter)
                            {
                                fprintf(stderr,"got NUMERIC_EFFECTER Response, but NUMERIC_EFFECTER is not ready\n");
                                return;
                            }
                            if( resphdr->command == PLDM_SET_NUMERIC_EFFECTER_VALUE )
                            {
                                if(parseSetNumericEffecterResponseMsg(responsePtr, responseMsg.size() - sizeof(pldm_msg_hdr) - 1)<0)
                                {
                                    fprintf(stderr,"%s: Parse SetEffecterState Response Fail\n",effecter->effecterName.c_str());
                                    return;
                                }
                                else
                                {
                                    //Send GetEffecterState--Start
                                    uint8_t dstEid = 8;
                                    uint8_t msgTag = 1;
                                    uint8_t instance_id = 0;
                                    uint8_t compEffecterCnt = 0x1;

                                    fprintf(stderr,"Parse response of SET_NUMERIC_EFFECTER_VALUE successfully [%s]\n", effecter->effecterName.c_str());
                                    //Get instanceID for PLDM msg--start
                                    mutex_instanceID.lock();
                                    instance_id = instance_id_g++;

                                    map<uint8_t, string>::iterator iter;

                                    iter = instanceIDmap.find(instance_id);
                                    if(iter != instanceIDmap.end())
                                    {
                                        auto effecterName_m = iter->second ;
                                        instanceIDmap.erase(iter);
                                        if(debugP)
                                            fprintf(stderr,"%s: erase instance ID:%d for reusing in sensor_reading\n",effecterName_m.c_str(),instance_id);
                                    }

                                    instanceIDmap.insert(pair<uint8_t, string>(instance_id, effecter->effecterName));

                                    if(instance_id_g>0x1F)
                                        instance_id_g=0;
                                    mutex_instanceID.unlock();
                                    //Get instanceID for PLDM msg--End

                                    std::vector<uint8_t> requestMsg(sizeof(pldm_msg_hdr) + 2);
                                    auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
                                    auto rc = encode_get_numeric_effecter_value_req( instance_id, effecter->effecterId, request);

                                    if (rc != PLDM_SUCCESS)
                                    {
                                        std::cerr << "Failed to encode request message for state effect reading" << " rc = " << rc << "\n";
                                        return;
                                    }
                                    requestMsg.insert(requestMsg.begin(), MCTP_MSG_TYPE_PLDM);
                                    auto method = systemBus->new_method_call("xyz.openbmc_project.MCTP-smbus", "/xyz/openbmc_project/mctp",
                                                                      "xyz.openbmc_project.MCTP.Base", "SendMctpMessagePayload");
                                    method.append(dstEid, msgTag, true, requestMsg);
                                    systemBus->call_noreply(method);
                                    fprintf(stderr,"Send Get Numeric Effecter Value request after getting a set response\n");
                                    //Resend GetNumericEffecterValue--End
                                }
                            }
                            else if( resphdr->command == PLDM_GET_NUMERIC_EFFECTER_VALUE )
                            {
                                uint8_t reteffecter_dataSize;
                                uint8_t reteffecter_operState;
                                uint8_t retpendingValue[4]={0};
                                uint8_t retpresentValue[4]={0};
                                uint32_t pendingValue;
                                uint32_t presentValue;

                                if(parseGetNumericEffecterResponseMsg(responsePtr, responseMsg.size() - sizeof(pldm_msg_hdr) - 1,
                                    &reteffecter_dataSize, &reteffecter_operState, retpendingValue, retpresentValue)<0)
                                {
                                    fprintf(stderr,"%s: Parse Ge tNumeric Effecter Value Response Fail\n",effecter->effecterName.c_str());
                                    return;
                                }
                                else
                                {
                                    fprintf(stderr,"Parse response of GetNumericEffecter value successfully [%s]\n",effecter->effecterName.c_str());
                                    pendingValue = *(reinterpret_cast<uint32_t*>(retpendingValue));
                                    presentValue = *(reinterpret_cast<uint32_t*>(retpresentValue));

                                    if(debugP)
                                    {
                                        fprintf(stderr,"    reteffecter_dataSize:%x\n",reteffecter_dataSize);
                                        fprintf(stderr,"    sensor_op_state:%x\n", reteffecter_operState);
                                        fprintf(stderr,"    pendingValue:%d\n", pendingValue);
                                        fprintf(stderr,"    presentValue:%d\n", presentValue);
                                    }

                                    effecter->effecterInterface->set_property("effecterOperationalState", reteffecter_operState);
                                    effecter->effecterInterface->set_property("pendingValue", pendingValue);
                                    effecter->effecterInterface->set_property("presentValue", presentValue);
                                    return;
                                }
                            }
                        }
                        /* Numeric Sensor
                         *    Numeric Sensor commands
                         *    PLDM_SET_NUMERIC_SENSOR_ENABLE = 0x10,
                         *    PLDM_GET_SENSOR_READING = 0x11,
                         *    PLDM_GET_SENSOR_THRESHOLD = 0x12,
                         *    PLDM_SET_SENSOR_THRESHOLD = 0x13,
                         *    PLDM_GET_SENSOR_HYSTERESIS = 0x15,
                         *    PLDM_SET_SENSOR_HYSTERESIS = 0x16,
                         */
                        else if( resphdr->command>=PLDM_SET_NUMERIC_SENSOR_ENABLE && resphdr->command<=PLDM_SET_SENSOR_HYSTERESIS )
                        {
                            auto& sensor = sensors[sensorName];
                            if(!sensor)
                            {
                                fprintf(stderr,"got NUMERIC_SENSOR Response, but NUMERIC_SENSOR is not ready\n");
                                return;
                            }
                            if(debugP)
                                fprintf(stderr,"%s: Resp instance_id:%x sensor->cmd:%d\n",sensor->sensorName.c_str(),instance_id,sensor->cmd);
                            if(sensor->cmd == GetSensorReading)
                            {
                                auto resphdr = reinterpret_cast<const pldm_msg_hdr*>(responsePtr);
                                if ( resphdr->command == PLDM_GET_SENSOR_READING )
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
                            }
                            else if(sensor->cmd == SetNumericSensorEnable)
                            {
                                fprintf(stderr,"Response is SetNumericSensorEnable\n");
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
                        }
                        /* Stste Sensor
                         *    State Sensor commands
                         *    PLDM_SET_STATE_SENSOR_ENABLE = 0x20,
                         *    PLDM_GET_STATE_SENSOR_READINGS = 0x21,
                         */
                        else if( resphdr->command>=PLDM_SET_STATE_SENSOR_ENABLE && resphdr->command<=PLDM_GET_STATE_SENSOR_READINGS )
                        {
                            auto& sensor = state_sensors[sensorName];
                            if(!sensor)
                            {
                                fprintf(stderr,"got STATE_SENSOR Response, but STATE_SENSOR is not ready\n");
                                return;
                            }
                            if(debugP)
                                fprintf(stderr,"%s: Resp instance_id:%x\n",sensor->sensorName.c_str(),instance_id);
                            if(sensor->cmd == GetStateSensorReading)
                            {
                                auto resphdr = reinterpret_cast<const pldm_msg_hdr*>(responsePtr);
                                if ( resphdr->command == PLDM_GET_STATE_SENSOR_READINGS )
                                {
                                    double PRESENT_val;

                                    uint8_t retcomp_sensorCnt=1;
                                    get_sensor_state_field retstateField[8];
                                    if(parseStateSensorReadingResponseMsg(responsePtr, responseMsg.size() - sizeof(pldm_msg_hdr) - 1, retcomp_sensorCnt, retstateField)<0)
                                    {
                                        uint8_t CC = 0x1;
                                        fprintf(stderr,"%s: Parse sensorStateReading Response Fail\n",sensor->sensorName.c_str());
                                        return;
                                    }
                                    else
                                    {
                                        fprintf(stderr,"Parse response of Get state sensor State Response Successfully [%s]\n", sensor->sensorName.c_str());
                                        for(int i=0; i<retcomp_sensorCnt; i++)
                                        {
                                            sensor->stateSensorInterface->set_property("sensorOperationalState", retstateField[i].sensor_op_state);
                                            sensor->stateSensorInterface->set_property("present_state", retstateField[i].present_state);
                                            sensor->stateSensorInterface->set_property("previous_state", retstateField[i].previous_state);
                                            sensor->stateSensorInterface->set_property("event_state", retstateField[i].event_state);

                                        if(debugP)
                                        {
                                                fprintf(stderr,"%d:sensor_op_state:%x\n",i,retstateField[i].sensor_op_state);
                                                fprintf(stderr,"%d:present_state:%x\n",i,retstateField[i].present_state);
                                                fprintf(stderr,"%d:previous_state:%x\n",i,retstateField[i].previous_state);
                                                fprintf(stderr,"%d:event_state:%x\n",i,retstateField[i].event_state);
                                        }

                                        }
                                        return;
                                    }
                                }
                            }
                            else if(sensor->cmd == SetStateSensorEnable)
                            {
                                if(parseSetStateSensorEnableResponseMsg(responsePtr, responseMsg.size() - sizeof(pldm_msg_hdr) - 1)<0)
                                {
                                    fprintf(stderr,"%s: Parse SetStateSensorEnable Response Fail\n",sensor->sensorName.c_str());
                                    //sensor->init();
                                    return;
                                }
                                else
                                    sensor->cmd = GetStateSensorReading;

                                fprintf(stderr,"Parse SetStateSensorEnable successfully, Start state_sensor_read_loop : sensorId =%X\n",sensor->sensorName.c_str(),sensor->sensorId);
                                sensor->state_sensor_read_loop();
                                return;
                            }

                        }
                        else
                            fprintf(stderr,"Got non-supported Response CMD:%x in OPER_SENSORS state\n",resphdr->command);
                        return;
                    }
                }
                else
                {
                    fprintf(stderr,"Got response from MessageReceivedSignal(pldm_state=%d), but there is no matched item in map\n",pldm_state);
                    return;
                }
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

