#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <vision/application/readiness.hpp>
#include <vision/application/production.hpp>
using namespace vision::application;
TEST_CASE("fresh evidence must match run recipe epoch and every mandatory prerequisite") {
    Readiness readiness("run-a","sha256:recipe",4);
    CHECK(readiness.evaluate(100).blockers.size()==9);
    for(std::size_t n=0;n<requirement_names.size();++n){
        CHECK(readiness.observe(static_cast<Requirement>(n),{"run-a","sha256:recipe",4,100,200,true}));
        CHECK(readiness.evaluate(100).prerequisites_ready==(n+1==requirement_names.size()));
    }
    CHECK_FALSE(readiness.evaluate(99).prerequisites_ready);
    CHECK_FALSE(readiness.evaluate(200).prerequisites_ready);
    CHECK_FALSE(readiness.observe(Requirement::Safety,{"old-run","sha256:recipe",4,101,200,true}));
    CHECK_FALSE(readiness.observe(Requirement::Safety,{"run-a","other-recipe",4,101,200,true}));
    CHECK_FALSE(readiness.observe(Requirement::Safety,{"run-a","sha256:recipe",3,101,200,true}));
    CHECK_FALSE(readiness.observe(Requirement::Safety,{"run-a","sha256:recipe",4,99,200,true}));
    CHECK(readiness.observe(Requirement::Safety,{"run-a","sha256:recipe",4,101,200,false}));
    CHECK(readiness.evaluate(102).blockers==std::vector<std::string>{"safety"});
    CHECK_FALSE(production_integration_validated);
    ProductionController controller;
    CHECK_FALSE(controller.start(vision::contracts::Mode::Production));
    CHECK_FALSE(controller.accepts());
}
TEST_CASE("each required dependency can independently revoke readiness") {
    for(std::size_t missing=0;missing<requirement_names.size();++missing){
        Readiness readiness("run-a","recipe",1);
        for(std::size_t i=0;i<requirement_names.size();++i)
            readiness.observe(static_cast<Requirement>(i),{"run-a","recipe",1,10,20,i!=missing});
        const auto decision=readiness.evaluate(15);
        CHECK_FALSE(decision.prerequisites_ready);
        REQUIRE(decision.blockers.size()==1);
        CHECK(decision.blockers[0]==requirement_names[missing]);
    }
}
