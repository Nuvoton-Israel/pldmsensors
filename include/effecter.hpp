#pragma once

#include <sdbusplus/asio/object_server.hpp>

#include <limits>
#include <memory>
#include <string>
#include <vector>

constexpr const char* effecterStateInterface = "xyz.openbmc_project.Effecter.State";
constexpr const char* effecterNumericInterface = "xyz.openbmc_project.Effecter.Numeric";
struct Effecter
{
    Effecter(const std::string& name,
           std::shared_ptr<sdbusplus::asio::connection>& conn) :
        name(std::regex_replace(name, std::regex("[^a-zA-Z0-9_/]+"), "_")),
        dbusConnection(conn)
    {}
    virtual ~Effecter() = default;
    std::string name;
    bool overriddenState = false;
    bool internalSet = false;
    std::shared_ptr<sdbusplus::asio::dbus_interface> effecterInterface;
    uint8_t state = std::numeric_limits<uint8_t>::quiet_NaN();

    std::shared_ptr<sdbusplus::asio::connection> dbusConnection;

    void
        setInitialProperties(std::shared_ptr<sdbusplus::asio::connection>& conn,
                             const std::string label = std::string(),
                             size_t thresholdSize = 0)
    {
        if (!effecterInterface->initialize())
        {
            std::cerr << "error initializing effecter interface\n";
        }
    }

};

struct NumericEffecter
{
    NumericEffecter(const std::string& name,
           std::shared_ptr<sdbusplus::asio::connection>& conn) :
        name(std::regex_replace(name, std::regex("[^a-zA-Z0-9_/]+"), "_")),
        dbusConnection(conn)
    {}
    virtual ~NumericEffecter() = default;
    std::string name;
    bool overriddenState = false;
    bool internalSet = false;
    std::shared_ptr<sdbusplus::asio::dbus_interface> effecterInterface;
    uint8_t state = std::numeric_limits<uint8_t>::quiet_NaN();

    std::shared_ptr<sdbusplus::asio::connection> dbusConnection;

    void
        setInitialProperties(std::shared_ptr<sdbusplus::asio::connection>& conn,
                             const std::string label = std::string(),
                             size_t thresholdSize = 0)
    {
        if (!effecterInterface->initialize())
        {
            std::cerr << "error initializing NumericEffecter interface\n";
        }
        else
            fprintf(stderr,"NumericEffecter interface initializing is done.\n");
    }

};