#include <vision/serialization/json_codec.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <set>
#include <limits>

namespace vision::serialization {
using J = nlohmann::json;
using namespace contracts;
namespace {
J parse(std::string_view text) {
    if(text.empty() || text.size() > max_document_bytes) throw ProtocolError("Document byte limit");
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int depth, J::parse_event_t event, J& value) {
        if(depth > 32) throw ProtocolError("Document depth limit");
        if(event == J::parse_event_t::object_start) keys.emplace_back();
        if(event == J::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            throw ProtocolError("Duplicate JSON key");
        if(event == J::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    return J::parse(text.begin(), text.end(), callback);
}
void fields(const J& j, std::initializer_list<const char*> required, std::initializer_list<const char*> optional = {}) {
    if(!j.is_object()) throw ProtocolError("Object expected");
    std::set<std::string> allowed;
    for(const auto* key : required) {
        allowed.insert(key);
        if(!j.contains(key)) throw ProtocolError(std::string("Missing field: ") + key);
    }
    for(const auto* key : optional) allowed.insert(key);
    for(auto it = j.begin(); it != j.end(); ++it)
        if(!allowed.contains(it.key())) throw ProtocolError("Unknown field: " + it.key());
}
std::string text(const J& value, std::size_t max = 128) {
    if(!value.is_string()) throw ProtocolError("String expected");
    auto s = value.get<std::string>();
    if(s.empty() || s.size() > max || s.find('\0') != std::string::npos) throw ProtocolError("String length/content");
    return s;
}
std::string choice(const J& j, std::initializer_list<std::string_view> values) {
    auto s = text(j);
    if(std::find(values.begin(), values.end(), s) == values.end()) throw ProtocolError("Unknown enum: " + s);
    return s;
}
std::uint64_t u64(const J& j) { return parse_u64(text(j, 20)); }
std::uint32_t number(const J& j, std::uint32_t max) {
    if(!j.is_number_unsigned() && !(j.is_number_integer() && j.get<std::int64_t>() >= 0))
        throw ProtocolError("Nonnegative JSON integer expected");
    const auto value = j.get<std::uint64_t>();
    if(value > max) throw ProtocolError("Integer limit");
    return static_cast<std::uint32_t>(value);
}
bool boolean(const J& j) {
    if(!j.is_boolean()) throw ProtocolError("Boolean expected");
    return j.get<bool>();
}
double finite(const J& j) {
    if(!j.is_number()) throw ProtocolError("Number expected");
    const auto n = j.get<double>();
    if(!std::isfinite(n)) throw ProtocolError("Nonfinite number");
    return n;
}
void array(const J& j, std::size_t max) {
    if(!j.is_array() || j.size() > max) throw ProtocolError("Array limit/type");
}
void version(const J& j) {
    if(number(j, UINT32_MAX) != 1) throw ProtocolError("Unsupported schema/API major");
}
Correlation correlation(const J& j) {
    fields(j, {"run_id","worker_id","worker_epoch","inspection_id","check_id","task_id","attempt"});
    Correlation c{RunId(text(j["run_id"])), WorkerId(text(j["worker_id"])), u64(j["worker_epoch"]),
        InspectionId(text(j["inspection_id"])), CheckId(text(j["check_id"])), TaskId(text(j["task_id"])), u64(j["attempt"])};
    if(!c.worker_epoch || !c.attempt) throw ProtocolError("Positive epoch/attempt required");
    return c;
}
J correlation_json(const Correlation& c) {
    return {{"run_id",c.run_id.value()},{"worker_id",c.worker_id.value()},
        {"worker_epoch",std::to_string(c.worker_epoch)},{"inspection_id",c.inspection_id.value()},
        {"check_id",c.check_id.value()},{"task_id",c.task_id.value()},{"attempt",std::to_string(c.attempt)}};
}
constexpr std::string_view categories[] = {"Configuration","Protocol","Device","Execution","Resource","Persistence","Delivery","Internal"};
constexpr std::string_view retries[] = {"Never","AfterRecovery","Safe"};
template<std::size_t N> std::size_t enum_index(const J& j, const std::string_view (&names)[N]) {
    const auto value = text(j);
    for(std::size_t i=0; i<N; ++i) if(value == names[i]) return i;
    throw ProtocolError("Unknown enum");
}
template<std::size_t N> std::string_view enum_name(std::size_t index, const std::string_view (&names)[N]) {
    if(index >= N) throw ProtocolError("Unknown internal enum");
    return names[index];
}
Error error(const J& j) {
    fields(j, {"code","category","message","retryability","origin"}, {"correlation","vendor_code"});
    Error e{text(j["code"]),static_cast<ErrorCategory>(enum_index(j["category"], categories)),
        text(j["message"],2048),static_cast<Retryability>(enum_index(j["retryability"], retries)),text(j["origin"]),{},{}};
    if(j.contains("correlation")) e.correlation = correlation(j["correlation"]);
    if(j.contains("vendor_code")) e.vendor_code = text(j["vendor_code"],256);
    return e;
}
J error_json(const Error& e) {
    J j{{"code",e.code},{"category",enum_name(static_cast<std::size_t>(e.category),categories)},
        {"message",e.message},{"retryability",enum_name(static_cast<std::size_t>(e.retryability),retries)},{"origin",e.origin}};
    if(e.correlation) j["correlation"] = correlation_json(*e.correlation);
    if(e.vendor_code) j["vendor_code"] = *e.vendor_code;
    return j;
}
CheckResult check(const J& j) {
    fields(j, {"correlation","execution_state","quality","defects","measurements","elapsed_ns"},
        {"classification","error","model_hash","masks"});
    CheckResult r{correlation(j["correlation"])};
    const auto state = choice(j["execution_state"],{"Succeeded","Failed","Cancelled","TimedOut"});
    r.state = state == "Succeeded" ? TaskState::Succeeded : state == "Failed" ? TaskState::Failed :
        state == "Cancelled" ? TaskState::Cancelled : TaskState::TimedOut;
    const auto quality = choice(j["quality"],{"Unknown","OK","NG"});
    r.quality = quality == "OK" ? QualityVerdict::OK : quality == "NG" ? QualityVerdict::NG : QualityVerdict::Unknown;
    r.elapsed_ns = u64(j["elapsed_ns"]);
    array(j["defects"],4096);
    for(const auto& d : j["defects"]) {
        fields(d,{"class_id","label","score","box_xyxy","coordinate_space"});
        choice(d["coordinate_space"],{"original_pixels"});
        array(d["box_xyxy"],4);
        if(d["box_xyxy"].size()!=4) throw ProtocolError("Box must have four coordinates");
        r.defects.push_back({static_cast<std::int32_t>(number(d["class_id"],INT32_MAX)),text(d["label"],256),
            finite(d["score"]),finite(d["box_xyxy"][0]),finite(d["box_xyxy"][1]),finite(d["box_xyxy"][2]),finite(d["box_xyxy"][3])});
    }
    array(j["measurements"],256);
    for(const auto& m : j["measurements"]) {
        fields(m,{"name","value","unit"});
        r.measurements.push_back({text(m["name"]),finite(m["value"]),text(m["unit"],32)});
    }
    if(j.contains("error")) r.error = error(j["error"]);
    if(j.contains("classification")) r.classification = text(j["classification"],256);
    if(j.contains("model_hash")) r.model_hash = text(j["model_hash"]);
    if(j.contains("masks")) {
        array(j["masks"],64);
        for(const auto& mask:j["masks"]) {
            fields(mask,{"hash","width","height","instance","storage"});
            choice(mask["storage"],{"local_ephemeral"});
            r.masks.push_back({text(mask["hash"]),number(mask["width"],1048576),number(mask["height"],1048576),number(mask["instance"],63)});
        }
    }
    validate(r);
    return r;
}
J check_json(const CheckResult& r) {
    validate(r);
    J j{{"correlation",correlation_json(r.correlation)},{"execution_state",name(r.state)},{"quality",name(r.quality)},
        {"defects",J::array()},{"measurements",J::array()},{"elapsed_ns",std::to_string(r.elapsed_ns)}};
    for(const auto& d : r.defects)
        j["defects"].push_back({{"class_id",d.class_id},{"label",d.label},{"score",d.score},
            {"box_xyxy",{d.x1,d.y1,d.x2,d.y2}},{"coordinate_space","original_pixels"}});
    for(const auto& m : r.measurements) j["measurements"].push_back({{"name",m.name},{"value",m.value},{"unit",m.unit}});
    if(r.classification) j["classification"] = *r.classification;
    if(r.error) j["error"] = error_json(*r.error);
    if(r.model_hash) j["model_hash"] = *r.model_hash;
    if(!r.masks.empty()) {
        j["masks"]=J::array();
        for(const auto& mask:r.masks)j["masks"].push_back({{"hash",mask.hash},{"width",mask.width},
            {"height",mask.height},{"instance",mask.instance},{"storage","local_ephemeral"}});
    }
    return j;
}
template<class F> auto boundary(F&& f) {
    try { return f(); }
    catch(const ProtocolError&) { throw; }
    catch(const J::exception&) { throw ProtocolError("Malformed JSON, invalid type or numeric range"); }
    catch(const std::invalid_argument& e) { throw ProtocolError(e.what()); }
}
inspection::Rule rule(const J& j) {
    if(!j.is_object() || !j.contains("kind")) throw ProtocolError("Missing rule kind");
    const auto kind = text(j["kind"]);
    inspection::Rule result;
    if(kind == "forbidden_class") {
        fields(j,{"kind","class_id"});
        result = inspection::ForbiddenClass{static_cast<std::int32_t>(number(j["class_id"],INT32_MAX))};
    } else if(kind == "count_range") {
        fields(j,{"kind","class_id","minimum","maximum"});
        result = inspection::CountRange{static_cast<std::int32_t>(number(j["class_id"],INT32_MAX)),
            number(j["minimum"],UINT32_MAX),number(j["maximum"],UINT32_MAX)};
    } else if(kind == "allowed_classification") {
        fields(j,{"kind","labels"}); array(j["labels"],256);
        inspection::AllowedClassification r;
        for(const auto& label : j["labels"]) r.labels.push_back(text(label,256));
        result = std::move(r);
    } else if(kind == "measurement_range") {
        fields(j,{"kind","name","unit","minimum","maximum","include_minimum","include_maximum"});
        result = inspection::MeasurementRange{text(j["name"]),text(j["unit"],32),finite(j["minimum"]),finite(j["maximum"]),
            boolean(j["include_minimum"]),boolean(j["include_maximum"])};
    } else throw ProtocolError("Unknown rule kind");
    inspection::validate_rule(result);
    return result;
}
void parameter_value(const J& schema, const J& value) {
    const auto type=text(schema["type"]);
    if((type=="string" && !value.is_string()) || (type=="boolean" && !value.is_boolean()) ||
       (type=="number" && !value.is_number()) ||
       (type=="integer" && !value.is_number_integer() && !value.is_number_unsigned()))
        throw ProtocolError("Parameter type mismatch");
    if(type=="number" || type=="integer") {
        const auto n=finite(value);
        if(type=="integer" && (n < -9007199254740991.0 || n > 9007199254740991.0))
            throw ProtocolError("Integer parameter exceeds exact JSON client range; use string protocol for uint64 counters");
        if((schema.contains("minimum") && n<finite(schema["minimum"])) ||
           (schema.contains("maximum") && n>finite(schema["maximum"]))) throw ProtocolError("Parameter range");
    }
    if(type=="string") {
        const auto s=value.get<std::string>();
        if(s.size()>4096 || s.find('\0')!=std::string::npos ||
           (schema.contains("minLength") && s.size()<number(schema["minLength"],4096)) ||
           (schema.contains("maxLength") && s.size()>number(schema["maxLength"],4096)))
            throw ProtocolError("Parameter string length");
    }
    if(schema.contains("enum") && std::find(schema["enum"].begin(),schema["enum"].end(),value)==schema["enum"].end())
        throw ProtocolError("Parameter enum");
}
void parameter_schema(const J& schema) {
    fields(schema,{"type","properties","additionalProperties"},{"required"});
    choice(schema["type"],{"object"});
    if(boolean(schema["additionalProperties"])) throw ProtocolError("Unknown parameters must be rejected");
    if(!schema["properties"].is_object() || schema["properties"].size()>128) throw ProtocolError("Parameter count/type");
    for(auto it=schema["properties"].begin();it!=schema["properties"].end();++it) {
        (void)Id<struct ParameterTag>(it.key());
        const auto& s=it.value();
        fields(s,{"type"},{"default","minimum","maximum","minLength","maxLength","enum","description","unit","sensitive","mutable_during_run"});
        const auto type=choice(s["type"],{"string","integer","number","boolean"});
        if(s.contains("description")) text(s["description"],2048);
        if(s.contains("unit")) text(s["unit"],32);
        if(s.contains("sensitive")) boolean(s["sensitive"]);
        if(s.contains("mutable_during_run")) boolean(s["mutable_during_run"]);
        for(const auto* key:{"minimum","maximum"})
            if(s.contains(key)) { if(type!="number" && type!="integer") throw ProtocolError("Range for nonnumber"); finite(s[key]); }
        if(s.contains("minimum") && s.contains("maximum") && finite(s["minimum"])>finite(s["maximum"]))
            throw ProtocolError("Inverted parameter range");
        for(const auto* key:{"minLength","maxLength"})
            if(s.contains(key)) { if(type!="string") throw ProtocolError("Length for nonstring"); number(s[key],4096); }
        if(s.contains("minLength") && s.contains("maxLength") && s["minLength"]>s["maxLength"])
            throw ProtocolError("Inverted parameter length");
        if(s.contains("enum")) {
            array(s["enum"],256);
            if(s["enum"].empty()) throw ProtocolError("Empty parameter enum");
            std::set<std::string> seen;
            for(const auto& entry:s["enum"]) {
                if(!seen.insert(entry.dump()).second) throw ProtocolError("Duplicate parameter enum");
                parameter_value(s,entry);
            }
        }
        if(s.contains("default")) parameter_value(s,s["default"]);
    }
    if(schema.contains("required")) {
        array(schema["required"],128); std::set<std::string> seen;
        for(const auto& key:schema["required"]) {
            const auto name=text(key);
            if(!schema["properties"].contains(name) || !seen.insert(name).second) throw ProtocolError("Invalid required parameter");
        }
    }
}
} // namespace
CheckResult decode_check(std::string_view document) {return boundary([&]{return check(parse(document));});}
std::string encode_check(const CheckResult& value) {return boundary([&]{return check_json(value).dump();});}

ResultEnvelope decode_result(std::string_view document) {
    return boundary([&] {
        const auto j = parse(document);
        fields(j,{"schema_version","event_type","event_id","sequence","emitted_at_unix_ns","result"});
        version(j["schema_version"]); choice(j["event_type"],{"inspection.finalized"});
        const auto& v = j["result"];
        fields(v,{"run_id","inspection_id","workpiece_id","station_id","mode","recipe_hash","execution_state","quality","expected_checks","checks","reason"});
        InspectionResult r{RunId(text(v["run_id"])),InspectionId(text(v["inspection_id"])),
            WorkpieceId(text(v["workpiece_id"])),StationId(text(v["station_id"]))};
        const auto mode = choice(v["mode"],{"Demo","Replay","Production"});
        r.mode = mode == "Demo" ? Mode::Demo : mode == "Replay" ? Mode::Replay : Mode::Production;
        r.recipe_hash = text(v["recipe_hash"]);
        if(!valid_hash(r.recipe_hash)) throw ProtocolError("Invalid recipe hash");
        const auto s = choice(v["execution_state"],{"Completed","Failed","Cancelled","TimedOut"});
        r.state = s=="Completed" ? InspectionState::Completed : s=="Failed" ? InspectionState::Failed :
            s=="Cancelled" ? InspectionState::Cancelled : InspectionState::TimedOut;
        const auto q = choice(v["quality"],{"Unknown","OK","NG"});
        r.quality = q=="OK" ? QualityVerdict::OK : q=="NG" ? QualityVerdict::NG : QualityVerdict::Unknown;
        r.reason = text(v["reason"],2048);
        array(v["expected_checks"],64); array(v["checks"],64);
        std::map<std::string,bool> expected;
        bool required = false;
        for(const auto& spec : v["expected_checks"]) {
            fields(spec,{"check_id","required"});
            CheckId id(text(spec["check_id"])); const auto req=boolean(spec["required"]);
            if(!expected.emplace(id.value(),req).second) throw ProtocolError("Duplicate expected check");
            required |= req; r.expected_checks.push_back({std::move(id),req});
        }
        if(!required) throw ProtocolError("No required checks");
        std::set<std::string> received;
        bool bad = false, ng = false;
        for(const auto& value : v["checks"]) {
            auto c = check(value);
            const auto key = c.correlation.check_id.value();
            if(c.correlation.run_id != r.run_id || c.correlation.inspection_id != r.inspection_id ||
               !expected.contains(key) || !received.insert(key).second) throw ProtocolError("Result identity mismatch");
            if(expected.at(key)) { bad |= c.state != TaskState::Succeeded; ng |= c.quality == QualityVerdict::NG; }
            r.checks.push_back(std::move(c));
        }
        if(r.state == InspectionState::Completed) {
            for(const auto& [id, req] : expected) if(req && !received.contains(id)) bad=true;
            if(bad || r.quality != (ng ? QualityVerdict::NG : QualityVerdict::OK))
                throw ProtocolError("Completed result inconsistent with required checks");
        } else if(r.quality != QualityVerdict::Unknown) throw ProtocolError("Unsuccessful result must be Unknown");
        return ResultEnvelope{1,EventId(text(j["event_id"])),u64(j["sequence"]),u64(j["emitted_at_unix_ns"]),std::move(r)};
    });
}
std::string encode_result(const ResultEnvelope& e) {
    return boundary([&] {
        const auto& r=e.result;
        validate(r);
        constexpr std::string_view modes[]={"Demo","Replay","Production"};
        J v{{"run_id",r.run_id.value()},{"inspection_id",r.inspection_id.value()},{"workpiece_id",r.workpiece_id.value()},
            {"station_id",r.station_id.value()},{"mode",enum_name(static_cast<std::size_t>(r.mode),modes)},
            {"recipe_hash",r.recipe_hash},{"execution_state",name(r.state)},{"quality",name(r.quality)},
            {"reason",r.reason},{"expected_checks",J::array()},{"checks",J::array()}};
        for(const auto& spec:r.expected_checks) v["expected_checks"].push_back({{"check_id",spec.check_id.value()},{"required",spec.required}});
        for(const auto& c:r.checks) v["checks"].push_back(check_json(c));
        J j{{"schema_version",e.schema_version},{"event_type","inspection.finalized"},{"event_id",e.event_id.value()},
            {"sequence",std::to_string(e.sequence)},{"emitted_at_unix_ns",std::to_string(e.emitted_at_unix_ns)},{"result",std::move(v)}};
        auto output=j.dump(2);
        (void)decode_result(output); // Apply the same semantic boundary to outbound data.
        return output;
    });
}
inspection::Recipe decode_recipe(std::string_view document) {
    return boundary([&] {
        const auto j=parse(document);
        fields(j,{"schema_version","recipe_id","version","content_hash","deadline_ms","trigger_mode","association","checks","storage","outputs"});
        version(j["schema_version"]);
        inspection::Recipe r{text(j["recipe_id"]),text(j["version"]),text(j["content_hash"]),u64(j["deadline_ms"]),
            choice(j["trigger_mode"],{"Software","Hardware"}),choice(j["association"],{"TriggerId"}),{},{},{},{}};
        (void)Id<struct RecipeTag>(r.recipe_id);
        if(!valid_hash(r.content_hash) || !r.deadline_ms || r.deadline_ms > 600000) throw ProtocolError("Recipe hash/deadline invalid");
        fields(j["storage"],{"image_policy","traceability"});
        r.image_policy=choice(j["storage"]["image_policy"],{"none","ng_only","all"});
        r.traceability=choice(j["storage"]["traceability"],{"optional","required"});
        array(j["checks"],64); array(j["outputs"],32);
        std::set<std::string> ids;
        bool required=false;
        for(const auto& c:j["checks"]) {
            fields(c,{"check_id","camera_id","required","rule"},{"model_hash"});
            inspection::RecipeCheck spec{CheckId(text(c["check_id"])),CameraId(text(c["camera_id"])),boolean(c["required"]),rule(c["rule"]),{}};
            if(!ids.insert(spec.check_id.value()).second) throw ProtocolError("Duplicate recipe check");
            if(c.contains("model_hash")) { spec.model_hash=text(c["model_hash"]); if(!valid_hash(*spec.model_hash)) throw ProtocolError("Invalid model hash"); }
            required |= spec.required;
            r.checks.push_back(std::move(spec));
        }
        if(!required) throw ProtocolError("Recipe needs required checks");
        ids.clear();
        for(const auto& o:j["outputs"]) {
            OutputId id(text(o));
            if(!ids.insert(id.value()).second) throw ProtocolError("Duplicate output");
            r.outputs.push_back(std::move(id));
        }
        return r;
    });
}
PluginManifest decode_manifest(std::string_view document) {
    return boundary([&] {
        const auto j=parse(document);
        fields(j,{"schema_version","plugin_id","plugin_version","kind","sdk_api_version","build_id","platform","architecture",
            "compiler_abi","runtime_variant","entry_library","capabilities","threading_model","max_instances","parameters_schema",
            "dependencies","license_metadata"});
        version(j["schema_version"]); version(j["sdk_api_version"]);
        PluginManifest m{PluginId(text(j["plugin_id"])),text(j["plugin_version"]),
            choice(j["kind"],{"Camera","Algorithm","Communication","ObjectStorage","ResultOutput"}),text(j["build_id"]),
            choice(j["platform"],{"windows","linux"}),choice(j["architecture"],{"x64"}),text(j["compiler_abi"]),
            choice(j["runtime_variant"],{"Release","Debug"}),text(j["entry_library"],256),
            choice(j["threading_model"],{"Serialized","Reentrant","DedicatedThread"}),{},number(j["max_instances"],1024),{},{},{},{}};
        if(!m.max_instances || m.entry_library.find_first_of("/\\:") != std::string::npos ||
           m.entry_library=="." || m.entry_library==".." ||
           !(m.platform=="windows" ? m.entry_library.ends_with(".dll") : m.entry_library.ends_with(".so")))
            throw ProtocolError("Invalid plugin library or instance limit");
        array(j["capabilities"],64); array(j["dependencies"],64);
        auto strings=[&](const J& values, std::vector<std::string>& dest) {
            std::set<std::string> unique;
            for(const auto& v:values) { auto s=text(v); if(!unique.insert(s).second) throw ProtocolError("Duplicate list entry"); dest.push_back(std::move(s)); }
        };
        strings(j["capabilities"],m.capabilities); strings(j["dependencies"],m.dependencies);
        parameter_schema(j["parameters_schema"]);
        m.parameters_schema=j["parameters_schema"].dump();
        fields(j["license_metadata"],{"spdx","file"});
        m.license_spdx=text(j["license_metadata"]["spdx"]);
        m.license_file=text(j["license_metadata"]["file"]);
        if(m.license_file.find_first_of("/\\:")!=std::string::npos || m.license_file=="." || m.license_file=="..")
            throw ProtocolError("License file must be package-local filename");
        return m;
    });
}
void validate_parameters(const PluginManifest& manifest, std::string_view document, bool while_running) {
    boundary([&] {
        const auto schema=parse(manifest.parameters_schema);
        parameter_schema(schema);
        const auto values=parse(document);
        if(!values.is_object()) throw ProtocolError("Parameter object required");
        if(!while_running && schema.contains("required")) for(const auto& key:schema["required"])
            if(!values.contains(key.get<std::string>())) throw ProtocolError("Missing required parameter");
        for(auto it=values.begin();it!=values.end();++it) {
            if(!schema["properties"].contains(it.key())) throw ProtocolError("Unknown parameter");
            const auto& s=schema["properties"][it.key()];
            if(while_running && (!s.contains("mutable_during_run") || !boolean(s["mutable_during_run"])))
                throw ProtocolError("Parameter immutable while running");
            parameter_value(s,it.value());
        }
        return true;
    });
}
FrameDescriptor decode_frame(std::string_view document,std::uint64_t slot_bytes) {
    return boundary([&] {
        const auto j=parse(document);
        fields(j,{"schema_version","run_id","pool_id","worker_id","worker_epoch","slot_id","slot_generation",
            "lease_id","frame_id","width","height","stride","offset","length","pixel_format"});
        version(j["schema_version"]);
        const auto format=choice(j["pixel_format"],{"Mono8","RGB8","BGR8"});
        const auto index=u64(j["slot_id"]);
        if(index>UINT32_MAX)throw ProtocolError("Slot index overflow");
        FrameDescriptor d{{RunId(text(j["run_id"])),PoolId(text(j["pool_id"])),
            {WorkerId(text(j["worker_id"])),u64(j["worker_epoch"])},static_cast<std::uint32_t>(index),
            u64(j["slot_generation"]),u64(j["lease_id"])},FrameId(text(j["frame_id"])),
            {number(j["width"],UINT32_MAX),number(j["height"],UINT32_MAX),u64(j["stride"]),u64(j["offset"]),u64(j["length"]),
            format=="Mono8"?PixelFormat::Mono8:format=="RGB8"?PixelFormat::RGB8:PixelFormat::BGR8}};
        if(!d.permit.owner.epoch||!d.permit.generation||!d.permit.lease)throw ProtocolError("Zero frame identity");
        validate_layout(d.layout,slot_bytes);return d;
    });
}
std::string encode_frame(const FrameDescriptor& d,std::uint64_t slot_bytes) {
    const auto& p=d.permit;const auto& l=d.layout;
    const auto result=J{{"schema_version",1},{"run_id",p.run.value()},{"pool_id",p.pool.value()},
        {"worker_id",p.owner.worker.value()},{"worker_epoch",std::to_string(p.owner.epoch)},
        {"slot_id",std::to_string(p.slot)},{"slot_generation",std::to_string(p.generation)},{"lease_id",std::to_string(p.lease)},
        {"frame_id",d.frame.value()},{"width",l.width},{"height",l.height},{"stride",std::to_string(l.stride)},
        {"offset",std::to_string(l.offset)},{"length",std::to_string(l.length)},
        {"pixel_format",l.format==PixelFormat::Mono8?"Mono8":l.format==PixelFormat::RGB8?"RGB8":l.format==PixelFormat::BGR8?"BGR8":"Invalid"}}.dump();
    (void)decode_frame(result,slot_bytes);return result;
}
} // namespace vision::serialization
