#include "../common/mysql.hpp"
namespace {
class Output final:public vision::outputs::AsyncOutput {
public:
    ~Output(){finish();}
private:
    void configure(const nlohmann::json& j) override {
        config_=vision::database::Config::parse(j);vision::database::Mysql::initialize_library();
    }
    void write(const nlohmann::json& request) override {
        vision::database::Deadline deadline(config_,request,stopped_);
        vision::database::Mysql db(config_,deadline);
        vision::database::write_event(db,request,config_);
    }
    std::string write_error(const std::exception& e) const override {
        return dynamic_cast<const vision::database::Error*>(&e)?e.what():"DB.WRITE_FAILED";
    }
    vision::database::Config config_;
};
}
#define VISION_OUTPUT_ID "vision.mysql-output"
#define VISION_OUTPUT_BUILD "mysql-output-v1"
#include "../../output-common/exports.hpp"
