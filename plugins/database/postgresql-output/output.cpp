#include "../common/postgresql.hpp"
namespace {
class Output final:public vision::outputs::AsyncOutput {
public:
    ~Output(){finish();}
private:
    void configure(const nlohmann::json& j) override {config_=vision::database::Config::parse(j);}
    void write(const nlohmann::json& request) override {
        vision::database::Deadline deadline(config_,request,stopped_);
        vision::database::Postgres db(config_,deadline);
        vision::database::write_event(db,request,config_);
    }
    std::string write_error(const std::exception& e) const override {
        return dynamic_cast<const vision::database::Error*>(&e)?e.what():"DB.WRITE_FAILED";
    }
    vision::database::Config config_;
};
}
#define VISION_OUTPUT_ID "vision.postgresql-output"
#define VISION_OUTPUT_BUILD "postgresql-output-v1"
#include "../../output-common/exports.hpp"
