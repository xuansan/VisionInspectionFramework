#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <httplib.h>
#include <curl/curl.h>
#include <sqlite3.h>
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QEventLoop>
#include <QImage>
#include <QLabel>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPixmap>
#include <QTimer>
#include <QUuid>
#include <array>
#include <chrono>
#include <filesystem>
#include <thread>

TEST_CASE("Qt Widgets offscreen render and PNG") {
    QLabel label("Vision SDK check");
    label.resize(240, 80);
    const QPixmap snapshot = label.grab();
    CHECK_FALSE(snapshot.isNull());
    QByteArray png;
    QBuffer stream(&png);
    REQUIRE(stream.open(QIODevice::WriteOnly));
    CHECK(snapshot.save(&stream, "PNG"));
    CHECK(png.startsWith("\x89PNG"));
}

TEST_CASE("Qt local socket roundtrip with deadline") {
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    REQUIRE(server.listen("vision-sdk-" + QUuid::createUuid().toString(QUuid::Id128)));
    QLocalSocket client;
    QEventLoop loop;
    bool received = false;
    QObject::connect(&server, &QLocalServer::newConnection, &loop, [&] {
        auto* socket = server.nextPendingConnection();
        QObject::connect(socket, &QLocalSocket::readyRead, socket, [socket] {
            socket->write(socket->readAll());
        });
    });
    QObject::connect(&client, &QLocalSocket::connected, &loop, [&] { client.write("ping"); });
    QByteArray response;
    QObject::connect(&client, &QLocalSocket::readyRead, &loop, [&] {
        response += client.readAll();
        if (response == "ping") { received = true; loop.quit(); }
    });
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(3000);
    client.connectToServer(server.serverName());
    loop.exec();
    CHECK(received);
}

TEST_CASE("OpenCV image processing and PNG roundtrip") {
    cv::Mat image(32, 32, CV_8UC1, cv::Scalar(180)), mask;
    cv::threshold(image, mask, 100, 255, cv::THRESH_BINARY);
    CHECK(cv::countNonZero(mask) == 1024);
    std::vector<unsigned char> encoded;
    REQUIRE(cv::imencode(".png", image, encoded));
    CHECK(cv::norm(image, cv::imdecode(encoded, cv::IMREAD_GRAYSCALE)) == 0);
}

TEST_CASE("ONNX Runtime CPU performs official multiply fixture") {
    const auto* api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    REQUIRE(api != nullptr);
    REQUIRE(std::string(OrtGetApiBase()->GetVersionString()) == "1.30.0");
    Ort::InitApi(api);
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "sdk-check");
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1);
    options.SetInterOpNumThreads(1);
    const std::filesystem::path model(qEnvironmentVariable("VISION_TEST_MODEL").toStdWString());
    REQUIRE(std::filesystem::is_regular_file(model));
    Ort::Session session(env, model.c_str(), options);
    REQUIRE(session.GetInputCount() == 1);
    REQUIRE(session.GetOutputCount() == 1);
    Ort::AllocatorWithDefaultOptions allocator;
    auto inputName = session.GetInputNameAllocated(0, allocator);
    auto outputName = session.GetOutputNameAllocated(0, allocator);
    const std::array<int64_t, 2> shape{3, 2};
    std::array<float, 6> input{1, 2, 3, 4, 5, 6};
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(),
        shape.data(), shape.size());
    const char* inputs[]{inputName.get()};
    const char* outputs[]{outputName.get()};
    auto values = session.Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
    REQUIRE(values[0].GetTensorTypeAndShapeInfo().GetElementCount() == 6);
    const float* result = values[0].GetTensorData<float>();
    for (size_t i = 0; i < input.size(); ++i) {
        CHECK(result[i] == doctest::Approx(input[i] * input[i]));
    }
}

TEST_CASE("SQLite transaction, JSON and logging") {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(":memory:", &raw) == SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    REQUIRE(sqlite3_exec(raw, "CREATE TABLE results(id TEXT PRIMARY KEY, value TEXT);"
        "BEGIN; INSERT INTO results VALUES('one','ok'); COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK);
    CHECK(sqlite3_exec(raw, "INSERT INTO results VALUES('one','ng');",
        nullptr, nullptr, nullptr) == SQLITE_CONSTRAINT);
    const auto json = nlohmann::json::parse(R"({"id":"18446744073709551615","quality":"OK"})");
    CHECK(json.at("id").get<std::string>() == "18446744073709551615");
    spdlog::info("SQLite {} and JSON verified", sqlite3_libversion());
}

TEST_CASE("HTTP library loopback request") {
    httplib::Server server;
    server.new_task_queue = [] { return new httplib::ThreadPool(2); };
    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::jthread thread([&] { server.listen_after_bind(); });
    struct Stop { httplib::Server& s; ~Stop() { s.stop(); } } stop{server};
    for (int i = 0; i < 100 && !server.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2);
    client.set_read_timeout(2);
    auto result = client.Get("/health");
    REQUIRE(result);
    CHECK(result->status == 200);
    CHECK(nlohmann::json::parse(result->body).at("status") == "ok");
}

TEST_CASE("libcurl native TLS and AWS signing option available") {
    REQUIRE(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    const auto* info = curl_version_info(CURLVERSION_NOW);
    REQUIRE(info->ssl_version != nullptr);
    CHECK(std::string(info->ssl_version).find("Schannel") != std::string::npos);
    CURL* handle = curl_easy_init();
    REQUIRE(handle != nullptr);
    CHECK(curl_easy_setopt(handle, CURLOPT_AWS_SIGV4, "aws:amz:us-east-1:s3") == CURLE_OK);
    curl_easy_cleanup(handle);
    curl_global_cleanup();
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    doctest::Context tests;
    tests.applyCommandLine(argc, argv);
    return tests.run();
}
