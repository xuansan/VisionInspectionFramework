#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <chrono>
#define NOMINMAX
#include <windows.h>
int main(){
    std::string input;char ch;while(std::cin.get(ch))input+=ch;
    const char* mode=std::getenv("VISION_TEST_ARCHIVE_FAULT");
    if(!mode)return 2;
    const std::string fault=mode;
    if(fault=="crash")TerminateProcess(GetCurrentProcess(),41);
    if(fault=="overflow"){std::cout<<std::string(16384,'x')<<std::flush;return 0;}
    if(fault=="malformed"){std::cout<<"{}";return 0;}
    for(;;)std::this_thread::sleep_for(std::chrono::milliseconds(20));
}
