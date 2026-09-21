#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <httplib.h>
#include <sqlite3.h>
#include <curl/curl.h>
#include <onnxruntime_c_api.h>
#include <string>

TEST_CASE("Installed SDK consumer uses exported libraries and headers") {
    CHECK(std::string(sqlite3_libversion()) == "3.53.4");
    CHECK(nlohmann::json::parse("{\"installed\":true}").at("installed") == true);
    CHECK(std::string(OrtGetApiBase()->GetVersionString()) == "1.30.0");
    REQUIRE(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    CHECK(std::string(curl_version_info(CURLVERSION_NOW)->version) == "8.22.0");
    curl_global_cleanup();
    httplib::Client client("127.0.0.1", 1);
    client.set_connection_timeout(1);
    spdlog::info("Installed SDK headers and libraries linked successfully");
}
