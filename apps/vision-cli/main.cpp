#include <vision/application/build_info.hpp>
#include <vision/application/demo.hpp>
#include <vision/serialization/json_codec.hpp>
#include <vision/ipc/message.hpp>
#include <fstream>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    try {
    const auto info = vision::application::build_info();
    if(argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << info.version << '\n';
        return 0;
    }
    if(argc == 2 && std::string_view(argv[1]) == "--status") {
        std::cout << "stage=" << info.stage << "\ninspection_available=false\ndemo_available=true\n";
        return 0;
    }
    if(argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
        std::cout << "vision-cli --version | --status | --help\n"
                     "  --demo ok|ng|failure|timeout|overload\n"
                     "  --validate-recipe <file> | --validate-manifest <file> | --validate-result <file> | --validate-message <file>\n"
                     "Synthetic core demo only; hardware inspection and production are unavailable.\n";
        return 0;
    }
    if(argc == 3 && std::string_view(argv[1]) == "--demo") {
        using S = vision::application::DemoScenario;
        const std::string_view arg(argv[2]);
        const auto scenario = arg=="ok" ? S::Ok : arg=="ng" ? S::Ng : arg=="failure" ? S::Failure :
            arg=="timeout" ? S::Timeout : arg=="overload" ? S::Overload : throw std::invalid_argument("Unknown demo scenario");
        const auto outcome=vision::application::run_demo(scenario);
        if(outcome.event) std::cout << vision::serialization::encode_result(*outcome.event) << '\n';
        else std::cout << "admitted=false\nreason=CapacityExceeded\n";
        return 0; // Command executed; quality/state is in the result, not the process exit code.
    }
    if(argc == 3 && (std::string_view(argv[1])=="--validate-recipe" ||
        std::string_view(argv[1])=="--validate-manifest" || std::string_view(argv[1])=="--validate-result" ||
        std::string_view(argv[1])=="--validate-message")) {
        std::ifstream file(argv[2],std::ios::binary);
        if(!file) throw std::invalid_argument("Cannot open document");
        std::string doc(vision::serialization::max_document_bytes+1,'\0');
        file.read(doc.data(),static_cast<std::streamsize>(doc.size()));
        doc.resize(static_cast<std::size_t>(file.gcount()));
        if(std::string_view(argv[1])=="--validate-recipe") (void)vision::serialization::decode_recipe(doc);
        else if(std::string_view(argv[1])=="--validate-manifest") (void)vision::serialization::decode_manifest(doc);
        else if(std::string_view(argv[1])=="--validate-message") (void)vision::ipc::decode_message(doc);
        else (void)vision::serialization::decode_result(doc);
        std::cout << "valid\n";
        return 0;
    }
    std::cerr << "Unsupported command. Inspection is not implemented; use --help.\n";
    return 2;
    } catch(const std::exception& e) {
        std::cerr << "Rejected: " << e.what() << '\n';
        return 2;
    }
}
