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
#include <systemd/sd-bus.h>

#include <sdbusplus/server.hpp>

#include "PLDMSensor.hpp"

#include <boost/algorithm/string/replace.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <iostream>

#include <exception>

int parseSensorReadingResponseMsg(pldm_msg* responsePtr, size_t payloadLength, double *PRESENT_val)
{
    auto resphdr = reinterpret_cast<const pldm_msg_hdr*>(responsePtr);
    if ( resphdr->command == 0x11)
    {
        // Response Header:3bytes
        //    Byte1:Rq(7),D(6),Instance_ID(0-4)
        //    Byte2:Hdr Version(6-7),PLDM Type(0-5) 0x2:Platform_monitoring_and control
        //    Byte3:PLDM Command Code 0x11:GetSensorReading ; 0x12:GetSensorThresholds
        // payload::
        //    completionCode: enum8
        //    sensorDataSize:enum8
        //    sensorOperationState:enum8
        //    sensorEventMessageEnable:enum8
        //    presentState:enum8
        //    previouState:enum8
        //    eventState:enum8
        //    presentReading:unit8/sint8/uint16/sint16/uint32/sint32
        //    00 01  00 02 11  00 01 00 02 0a 07 0a 74
        uint8_t retcompletionCode;
        uint8_t retsensor_dataSize=PLDM_SENSOR_DATA_SIZE_SINT32;
        uint8_t retsensor_operationalState;
        uint8_t retsensor_event_messageEnable;
        uint8_t retpresentState;
        uint8_t retpreviousState;
        uint8_t reteventState;
        uint8_t retpresentReading[4];

        auto rc = decode_get_sensor_reading_resp(
            responsePtr, payloadLength, &retcompletionCode,
            &retsensor_dataSize, &retsensor_operationalState,
            &retsensor_event_messageEnable, &retpresentState, &retpreviousState,
            &reteventState, reinterpret_cast<uint8_t*>(&retpresentReading));
        if (rc != PLDM_SUCCESS || retcompletionCode != PLDM_SUCCESS)
        {
            std::cerr << "Response Message Error: "
                    << "rc=" << rc << ",retcompletionCode=" << static_cast<int>(retcompletionCode) << "\n";
            return -1;
        }

        if (retsensor_dataSize == PLDM_EFFECTER_DATA_SIZE_UINT8 ||
            retsensor_dataSize == PLDM_EFFECTER_DATA_SIZE_SINT8) {
            *PRESENT_val = retpresentReading[0];
            return 0;
        }
        else if (retsensor_dataSize == PLDM_EFFECTER_DATA_SIZE_UINT16 ||
            retsensor_dataSize == PLDM_EFFECTER_DATA_SIZE_SINT16)
        {
            uint16_t val16 = 0;
            memcpy(&val16, retpresentReading, 2);
            *PRESENT_val = val16 = le16toh(val16);
            return 0;
        }
        else if (retsensor_dataSize == PLDM_EFFECTER_DATA_SIZE_UINT32 ||
            retsensor_dataSize == PLDM_EFFECTER_DATA_SIZE_SINT32)
        {
            uint32_t val32 = 0;
            memcpy(&val32, retpresentReading, 4);
            *PRESENT_val = val32 = le32toh(val32);
            return 0;
        }
        else
            printf("retsensor_dataSize is not as expected:%d\n",retsensor_dataSize);
        return -1;

    }
    else
        fprintf(stderr,"Response command is not 0x11(%x)\n",resphdr->command);

    return -1;
}

std::pair<int, std::vector<uint8_t>> PLDMSensor::createGetSensorReadingRequestMsg(uint16_t sensorId, bool8_t rearmEventState)
{
    std::vector<uint8_t> requestMsg(
        sizeof(pldm_msg_hdr) + 3);

    auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
    auto rc = encode_get_sensor_reading_req(instance_id, sensorId, rearmEventState,
                                               request);

    return {rc, requestMsg};
}

/*
 * print the input buffer if pldm verbosity is enabled.
 *
 */
void printBuffer(const std::vector<uint8_t>& buffer, bool pldmVerbose)
{
    if (pldmVerbose && !buffer.empty())
    {
        std::ostringstream tempStream;
        std::string str;
        for (int byte : buffer)
        {
            std::stringstream ss;
            ss << std::setfill('0') << std::setw(2) << std::hex << byte;
            str += ss.str()+" ";
        }
        std::cout <<"printBuffer:"<<str.length()<<":"<< str <<"\n";
    }
}

std::pair<int, std::vector<uint8_t>> PLDMSensor::createSetSensorThresholdRequestMsg(uint16_t sensorId, uint8_t sensorDataSize, uint32_t THRESHOLDs_val[])
{
    uint8_t thresholds[24] = {};
    int size = 0;
    if (sensorDataSize == PLDM_EFFECTER_DATA_SIZE_UINT8 ||
        sensorDataSize == PLDM_EFFECTER_DATA_SIZE_SINT8) {
        size = 1;
         for(int i=0;i<6;i++)
            thresholds[i] = THRESHOLDs_val[i];
    }
    else if (sensorDataSize == PLDM_EFFECTER_DATA_SIZE_UINT16 ||
        sensorDataSize == PLDM_EFFECTER_DATA_SIZE_SINT16)
    {
        size = 2;
        for(int i=0;i<6;i++)
        {
            THRESHOLDs_val[i] = htole16(THRESHOLDs_val[i]);
            memcpy(&thresholds[i*2], &THRESHOLDs_val[i], 2);

        }
    }
    else if (sensorDataSize == PLDM_EFFECTER_DATA_SIZE_UINT32 ||
        sensorDataSize == PLDM_EFFECTER_DATA_SIZE_SINT32)
    {
        size = 4;
        for(int i=0;i<6;i++)
        {
            THRESHOLDs_val[i] = htole32(THRESHOLDs_val[i]);
            memcpy(&thresholds[i*4], &THRESHOLDs_val[i], 4);
        }
    }

    std::vector<uint8_t> requestMsg(
        sizeof(pldm_msg_hdr) + 3 + 6*size);
    
    auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());

    auto rc = encode_set_sensor_threshold_req(instance_id, sensorId, sensorDataSize, thresholds, request);

    return {rc, requestMsg};
}

int parseGetThresholdResponseMsg(pldm_msg* responsePtr, size_t payloadLength, int THRESHOLDs_val[] )
{
    uint8_t cc = 0;
    auto resphdr = reinterpret_cast<const pldm_msg_hdr*>(responsePtr);
    if(resphdr->command == 0x12)
    {
        uint8_t sensorDataSize{};
        uint8_t sensorValue[24] = {};
        auto rc = decode_get_sensor_threshold_resp(responsePtr, payloadLength, &cc, &sensorDataSize,
            reinterpret_cast<uint8_t*>(&sensorValue));

        if (rc != PLDM_SUCCESS || cc != PLDM_SUCCESS)
        {
            std::cerr << "Response Message Error: "
                    << "rc=" << rc << ",cc=" << static_cast<int>(cc) << "\n";
            return -1;
        }

        if (sensorDataSize == PLDM_EFFECTER_DATA_SIZE_UINT8 ||
            sensorDataSize == PLDM_EFFECTER_DATA_SIZE_SINT8)
        {
            for(int i=0;i<6;i++)
                THRESHOLDs_val[i] = sensorValue[i];

        }
        else if (sensorDataSize == PLDM_EFFECTER_DATA_SIZE_UINT16 ||
            sensorDataSize == PLDM_EFFECTER_DATA_SIZE_SINT16)
        {
            for(int i=0;i<6;i++)
            {
                uint16_t val16 = 0;
                memcpy(&val16, &sensorValue[i*2], 2);
                THRESHOLDs_val[i] = le16toh(val16);
            }
        }
        else if (sensorDataSize == PLDM_EFFECTER_DATA_SIZE_UINT32 ||
            sensorDataSize == PLDM_EFFECTER_DATA_SIZE_SINT32)
        {
            for(int i=0;i<6;i++)
            {
                uint32_t val32 = 0;
                memcpy(&val32, &sensorValue[i*4], 4);
                THRESHOLDs_val[i] = le32toh(val32);
            }
        }
    }
    else
        fprintf(stderr,"GetThreshold Response command should be 0x12(%x)\n",resphdr->command);
    return -1;
}

std::pair<int, std::vector<uint8_t>> PLDMSensor::createGetSensorThresholdRequestMsg(uint16_t sensorId)
{
    std::vector<uint8_t> requestMsg(
        sizeof(pldm_msg_hdr) + 2);

    auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
    auto rc = encode_get_sensor_threshold_req(instance_id, sensorId, request);
    return {rc, requestMsg};
}

int parseSetTidResponseMsg(pldm_msg* responsePtr, size_t payloadLength)
{
    uint8_t cc = 0;
    auto resphdr = reinterpret_cast<const pldm_msg_hdr*>(responsePtr);

    if(resphdr->command == 0x1)
    {
        auto rc = decode_set_tid_resp(responsePtr, payloadLength, &cc);

        if (rc != PLDM_SUCCESS || cc != PLDM_SUCCESS)
        {
            std::cerr << "Response Message Error: "
                    << "rc=" << rc << ",cc=" << static_cast<int>(cc) << "\n";
            return -1;
        }
        fprintf(stderr,"SetTID successfully\n");
        return 0;
    }
    else
        fprintf(stderr,"Response command is not 0x1(%x)\n",resphdr->command);

    return -1;
}

std::pair<int, std::vector<uint8_t>> PLDMSensor::createSetNumericSensorEnableRequestMsg(uint16_t sensorId, uint8_t sensor_operational_state, uint8_t sensor_event_message_enable)
{
    std::vector<uint8_t> requestMsg(
        sizeof(pldm_msg_hdr) + 4);

    auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
    auto rc = encode_set_numeric_sensor_enable_req(instance_id, sensorId, sensor_operational_state, sensor_event_message_enable,
                                               request);

    return {rc, requestMsg};
}



std::pair<int, std::vector<uint8_t>> createSetTIDRequestMsg(uint8_t TID)
{
    std::vector<uint8_t> requestMsg(
        sizeof(pldm_msg_hdr) + 1);

    auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
    auto rc = encode_set_tid_req(0, TID, request);
    return {rc, requestMsg};
}

int parseSetThresholdResponseMsg(pldm_msg* responsePtr, size_t payloadLength)
{
    uint8_t cc = 0;
    auto resphdr = reinterpret_cast<const pldm_msg_hdr*>(responsePtr);

    if(resphdr->command == 0x13)
    {
        auto rc = decode_set_sensor_threshold_resp(responsePtr, payloadLength, &cc);

        if (rc != PLDM_SUCCESS || cc != PLDM_SUCCESS)
        {
            std::cerr << "Response Message Error: (SetThresholdResponse)"
                    << "rc=" << rc << ",cc=" << static_cast<int>(cc) << "\n";
            return -1;
        }
        fprintf(stderr,"SetThreshold Response successfully\n");
        return 0;
    }
    else
        fprintf(stderr,"Response command is not 0x13(%02x)\n",resphdr->command);

    return -1;
}

int parseSetNumericSensorEnableResponseMsg(pldm_msg* responsePtr, size_t payloadLength)
{
    uint8_t cc = 0;
    auto resphdr = reinterpret_cast<const pldm_msg_hdr*>(responsePtr);

    if(resphdr->command == 0x10)
    {
        auto rc = decode_set_numeric_sensor_enable_resp(responsePtr, payloadLength, &cc);

        if (rc != PLDM_SUCCESS || cc != PLDM_SUCCESS)
        {
            std::cerr << "Response Message Error: (set_numeric_sensor_enable)"
                    << "rc=" << rc << ",cc=" << static_cast<int>(cc) << "\n";
            return -1;
        }
        fprintf(stderr,"set_numeric_sensor_enable successfully\n");
        return 0;
    }
    else
        fprintf(stderr,"Response command is not 0x10(%02x)\n",resphdr->command);

    return -1;
}

int parseGetTidResponseMsg(pldm_msg* responsePtr, size_t payloadLength, uint8_t *tid)
{
    uint8_t cc = 0;
    auto resphdr = reinterpret_cast<const pldm_msg_hdr*>(responsePtr);

    if(resphdr->command == 0x2)
    {
        auto rc = decode_get_tid_resp(responsePtr, payloadLength, &cc, tid);

        if (rc != PLDM_SUCCESS || cc != PLDM_SUCCESS)
        {
            std::cerr << "Response Message Error: "
                    << "rc=" << rc << ",cc=" << static_cast<int>(cc) << "\n";
            return -1;
        }
        fprintf(stderr,"GetTID successfully\n");
        return 0;
    }
    else
        fprintf(stderr,"resphdr command is not 0x2(%x)\n",resphdr->command);
    return -1;
}

int matchPldmTypes(std::vector<bitfield8_t>& types, pldm_supported_types pldmType)
{
    for (int i = 0; i < PLDM_MAX_TYPES; i++)
    {
        bitfield8_t b = types[i / 8];
        if (b.byte & (1 << i % 8))
        {
            if( i==pldmType )
                return 1;
        }
    }
    return 0;

}

void printPldmTypes(std::vector<bitfield8_t>& types)
{
    std::cout << "Supported types:";
    for (int i = 0; i < PLDM_MAX_TYPES; i++)
    {
        bitfield8_t b = types[i / 8];
        if (b.byte & (1 << i % 8))
        {
            std::cout << " " << i;
            auto it = std::find_if(
                pldmTypes.begin(), pldmTypes.end(),
                [i](const auto& typePair) { return typePair.second == i; });
            if (it != pldmTypes.end())
            {
                std::cout << "(" << it->first << ")";
            }
        }
    }

    std::cout << std::endl;
}

int parseGetTypeResponseMsg(pldm_msg* responsePtr, size_t payloadLength, pldm_supported_types pldmType)
{
    uint8_t cc = 0;
    std::vector<bitfield8_t> types(8);
    auto resphdr = reinterpret_cast<const pldm_msg_hdr*>(responsePtr);

    if(resphdr->command == 0x4)
    {
        auto rc = decode_get_types_resp(responsePtr, payloadLength, &cc,
                                        types.data());
        if (rc != PLDM_SUCCESS || cc != PLDM_SUCCESS)
        {
            std::cerr << "Response Message Error of GetType: "
                      << "rc=" << rc << ",cc=" << static_cast<int>(cc) << "\n";
            return -1;
        }
        
        printPldmTypes(types);
        return matchPldmTypes(types, pldmType);
    }
    else
        fprintf(stderr,"resphdr command is not 0x4(%x)\n",resphdr->command);
    return -1;
}

std::pair<int, std::vector<uint8_t>> createGetTIDRequestMsg()
{
    std::vector<uint8_t> requestMsg(
        sizeof(pldm_msg_hdr));

    auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
    auto rc = encode_get_tid_req(0, request);
    return {rc, requestMsg};
}

std::pair<int, std::vector<uint8_t>> createGetTypeRequestMsg()
{
        std::vector<uint8_t> requestMsg(sizeof(pldm_msg_hdr));
        auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
        auto rc = encode_get_types_req(0, request);
        return {rc, requestMsg};
}

template <typename T>
int readMessageItem(const std::string& typeCode, sdbusplus::message::message& m,
                    nlohmann::json& data)
{
    T value;

    int r = sd_bus_message_read_basic(m.get(), typeCode.front(), &value);
    if (r < 0)
    {
        fprintf(stderr,"sd_bus_message_read_basic on type %s failed!\n",typeCode.c_str());
        return r;
    }

    data = value;
    return 0;
}

std::vector<std::string> dbusArgSplit(const std::string& string)
{
    std::vector<std::string> ret;
    if (string.empty())
    {
        return ret;
    }
    ret.emplace_back("");
    int containerDepth = 0;

    for (std::string::const_iterator character = string.begin();
         character != string.end(); character++)
    {
        ret.back() += *character;
        switch (*character)
        {
            case ('a'):
                break;
            case ('('):
            case ('{'):
                containerDepth++;
                break;
            case ('}'):
            case (')'):
                containerDepth--;
                if (containerDepth == 0)
                {
                    if (character + 1 != string.end())
                    {
                        ret.emplace_back("");
                    }
                }
                break;
            default:
                if (containerDepth == 0)
                {
                    if (character + 1 != string.end())
                    {
                        ret.emplace_back("");
                    }
                }
                break;
        }
    }

    return ret;
}

int readDictEntryFromMessage(const std::string& typeCode,
                             sdbusplus::message::message& m,
                             nlohmann::json& object)
{
    std::vector<std::string> types = dbusArgSplit(typeCode);
    if (types.size() != 2)
    {
         fprintf(stderr,"wrong number contained types in dictionary: %d\n", types.size());
        return -1;
    }

    int r = sd_bus_message_enter_container(m.get(), SD_BUS_TYPE_DICT_ENTRY,
                                           typeCode.c_str());
    if (r < 0)
    {
         fprintf(stderr,"sd_bus_message_enter_container with rc %d\n",r);
        return r;
    }

    nlohmann::json key;
    r = convertDBusToJSON(types[0], m, key);
    if (r < 0)
    {
        return r;
    }

    const std::string* keyPtr = key.get_ptr<const std::string*>();
    if (keyPtr == nullptr)
    {
        // json doesn't support non-string keys.  If we hit this condition,
        // convert the result to a string so we can proceed
        key = key.dump();
        keyPtr = key.get_ptr<const std::string*>();
        // in theory this can't fail now, but lets be paranoid about it
        // anyway
        if (keyPtr == nullptr)
        {
            return -1;
        }
    }
    nlohmann::json& value = object[*keyPtr];

    r = convertDBusToJSON(types[1], m, value);
    if (r < 0)
    {
        return r;
    }

    r = sd_bus_message_exit_container(m.get());
    if (r < 0)
    {
        fprintf(stderr,"sd_bus_message_exit_container failed\n");
        return r;
    }

    return 0;
}

int readArrayFromMessage(const std::string& typeCode,
                         sdbusplus::message::message& m, nlohmann::json& data)
{
    if (typeCode.size() < 2)
    {
        fprintf(stderr,"Type code %s too small for an array\n",typeCode.c_str());
        return -1;
    }

    std::string containedType = typeCode.substr(1);

    int r = sd_bus_message_enter_container(m.get(), SD_BUS_TYPE_ARRAY,
                                           containedType.c_str());
    if (r < 0)
    {
        fprintf(stderr,"sd_bus_message_enter_container failed with rc %d\n",r);
        return r;
    }

    bool dict = boost::starts_with(containedType, "{") &&
                boost::ends_with(containedType, "}");

    if (dict)
    {
        // Remove the { }
        containedType = containedType.substr(1, containedType.size() - 2);
        data = nlohmann::json::object();
    }
    else
    {
        data = nlohmann::json::array();
    }

    while (true)
    {
        r = sd_bus_message_at_end(m.get(), false);
        if (r < 0)
        {
            fprintf(stderr,"sd_bus_message_at_end failed\n");;
            return r;
        }

        if (r > 0)
        {
            break;
        }

        // Dictionaries are only ever seen in an array
        if (dict)
        {
            r = readDictEntryFromMessage(containedType, m, data);
            if (r < 0)
            {
                return r;
            }
        }
        else
        {
            data.push_back(nlohmann::json());

            r = convertDBusToJSON(containedType, m, data.back());
            if (r < 0)
            {
                return r;
            }
        }
    }

    r = sd_bus_message_exit_container(m.get());
    if (r < 0)
    {
        fprintf(stderr,"sd_bus_message_exit_container failed\n");
        return r;
    }

    return 0;
}

int convertDBusToJSON(const std::string& returnType,
                      sdbusplus::message::message& m, nlohmann::json& response)
{
    int r = 0;
    const std::vector<std::string> returnTypes = dbusArgSplit(returnType);

    for (const std::string& typeCode : returnTypes)
    {
        nlohmann::json* thisElement = &response;
        if (returnTypes.size() > 1)
        {
            response.push_back(nlohmann::json{});
            thisElement = &response.back();
        }

        if (typeCode == "s")
        {
            r = readMessageItem<char*>(typeCode, m, *thisElement);
            if (r < 0)
            {
                return r;
            }
        }
        else if (typeCode == "g")
        {
            r = readMessageItem<char*>(typeCode, m, *thisElement);
            if (r < 0)
            {
                return r;
            }
        }
        else if (typeCode == "o")
        {
            r = readMessageItem<char*>(typeCode, m, *thisElement);
            if (r < 0)
            {
                return r;
            }
        }
        else if (typeCode == "b")
        {
            r = readMessageItem<int>(typeCode, m, *thisElement);
            if (r < 0)
            {
                return r;
            }

            *thisElement = static_cast<bool>(thisElement->get<int>());
        }
        else if (typeCode == "u")
        {
            r = readMessageItem<uint32_t>(typeCode, m, *thisElement);
            if (r < 0)
            {
                return r;
            }
        }
        else if (typeCode == "i")
        {
            r = readMessageItem<int32_t>(typeCode, m, *thisElement);
            if (r < 0)
            {
                return r;
            }
        }
        else if (typeCode == "x")
        {
            r = readMessageItem<int64_t>(typeCode, m, *thisElement);
            if (r < 0)
            {
                return r;
            }
        }
        else if (typeCode == "t")
        {
            r = readMessageItem<uint64_t>(typeCode, m, *thisElement);
            if (r < 0)
            {
                return r;
            }
        }
        else if (typeCode == "n")
        {
            r = readMessageItem<int16_t>(typeCode, m, *thisElement);
            if (r < 0)
            {
                return r;
            }
        }
        else if (typeCode == "q")
        {
            r = readMessageItem<uint16_t>(typeCode, m, *thisElement);
            if (r < 0)
            {
                return r;
            }
        }
        else if (typeCode == "y")
        {
            r = readMessageItem<uint8_t>(typeCode, m, *thisElement);
            if (r < 0)
            {
                return r;
            }
        }
        else if (typeCode == "d")
        {
            r = readMessageItem<double>(typeCode, m, *thisElement);
            if (r < 0)
            {
                return r;
            }
        }
        else if (typeCode == "h")
        {
            r = readMessageItem<int>(typeCode, m, *thisElement);
            if (r < 0)
            {
                return r;
            }
        }
        else if (boost::starts_with(typeCode, "a"))
        {
            r = readArrayFromMessage(typeCode, m, *thisElement);
            if (r < 0)
            {
                return r;
            }
        }/*
        else if (boost::starts_with(typeCode, "(") &&
                 boost::ends_with(typeCode, ")"))
        {
            r = readStructFromMessage(typeCode, m, *thisElement);
            if (r < 0)
            {
                return r;
            }
        }
        else if (boost::starts_with(typeCode, "v"))
        {
            r = readVariantFromMessage(m, *thisElement);
            if (r < 0)
            {
                return r;
            }
        }*/
        else
        {
            fprintf(stderr, "Invalid D-Bus signature type %s\n",typeCode.c_str());
            return -2;
        }
    }

    return 0;
}