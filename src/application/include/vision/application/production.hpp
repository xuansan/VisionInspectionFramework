#pragma once
#include <vision/contracts/types.hpp>
#include <vision/application/readiness.hpp>
namespace vision::application {
enum class ProductionState {Stopped,Starting,Running,Paused,Draining,Faulted};
class ProductionController {
public:
    bool start(contracts::Mode mode) {
        if(mode!=contracts::Mode::Demo||state_!=ProductionState::Stopped)return false;
        state_=ProductionState::Starting;return true;
    }
    void ready(bool valid) {
        if(state_==ProductionState::Starting&&valid)state_=ProductionState::Running;
        else if(state_==ProductionState::Running&&!valid)state_=ProductionState::Faulted;
    }
    void pause() {if(state_==ProductionState::Running)state_=ProductionState::Paused;}
    bool resume(bool ready) {
        if(state_!=ProductionState::Paused||!ready)return false;
        state_=ProductionState::Running;return true;
    }
    void fault() {state_=ProductionState::Faulted;}
    void drain() {if(state_!=ProductionState::Stopped)state_=ProductionState::Draining;}
    void stopped() {state_=ProductionState::Stopped;}
    bool accepts() const {return state_==ProductionState::Running;}
    ProductionState state() const {return state_;}
private:
    ProductionState state_{ProductionState::Stopped};
};
}
