#include "Infrastructure/TestSupport.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

namespace ForgeConductor::Tests {
namespace {
using Json = nlohmann::json;
namespace Fs = std::filesystem;
Fs::path resources;
Json manifest;
Json contracts;

Json readJson(const Fs::path& file) {
    std::ifstream stream{file, std::ios::binary};
    require(static_cast<bool>(stream), "Starter contract fixture could not be opened.");
    return Json::parse(stream);
}

Json graphFor(const Json& starter, const char* field) {
    const auto file = starter.at(field).get<std::string>();
    require(Fs::path{file}.filename().string() == file && file.ends_with(".api.json"),
        "Starter graph filename leaves its resource directory.");
    return readJson(resources / file);
}

void inspectNode(const Json& graph, const std::string& id, std::set<std::string>& visiting,
    std::set<std::string>& visited) {
    if (visited.contains(id)) return;
    require(visiting.insert(id).second, "Starter graph contains a dependency cycle.");
    const auto& node = graph.at(id);
    require(node.is_object() && node.size() == 2U && node.contains("class_type") && node.contains("inputs"),
        "Starter node does not have the executable API shape.");
    const auto& schema = contracts.at(node.at("class_type").get<std::string>());
    const auto& inputs = node.at("inputs");
    require(inputs.is_object(), "Starter node inputs are not an object.");
    for (const auto& [name, definition] : schema.at("input").at("required").items()) {
        static_cast<void>(definition);
        require(inputs.contains(name), "Starter omitted a required installed node input.");
    }
    for (const auto& [name, value] : inputs.items()) {
        Json definition;
        for (const auto* group : {"required", "optional"}) {
            const auto& groupInputs = schema.at("input").at(group);
            if (groupInputs.contains(name)) definition = groupInputs.at(name);
        }
        require(!definition.is_null(), "Starter used an input absent from its installed contract.");
        if (value.is_array()) {
            require(value.size() == 2U && value[0].is_string() && value[1].is_number_unsigned(),
                "Starter link does not contain a node ID and unsigned output index.");
            const auto source = value[0].get<std::string>();
            require(graph.contains(source), "Starter link refers to an absent node.");
            const auto& outputTypes = contracts.at(graph.at(source).at("class_type").get<std::string>()).at("output");
            const auto index = value[1].get<std::size_t>();
            require(index < outputTypes.size(), "Starter link selects an absent output port.");
            require(definition[0].is_string() && definition[0] == outputTypes[index],
                "Starter connected incompatible installed node contracts.");
            inspectNode(graph, source, visiting, visited);
        } else if (definition[0].is_array()) {
            require(std::find(definition[0].begin(), definition[0].end(), value) != definition[0].end(),
                "Starter selected an unavailable model, sampler, scheduler, or format.");
        } else {
            const auto type = definition[0].get<std::string>();
            require((type == "STRING" && value.is_string()) || (type == "INT" && value.is_number_integer()) ||
                (type == "FLOAT" && value.is_number()) || (type == "BOOLEAN" && value.is_boolean()),
                "Starter literal does not match its installed primitive contract.");
            if (value.is_number() && definition.size() > 1U) {
                const auto numeric = value.get<double>();
                if (definition[1].contains("min")) require(numeric >= definition[1].at("min").get<double>(), "Starter input is below its installed minimum.");
                if (definition[1].contains("max")) require(numeric <= definition[1].at("max").get<double>(), "Starter input exceeds its installed maximum.");
            }
        }
    }
    visiting.erase(id);
    visited.insert(id);
}

void validateGraph(const Json& starter, const Json& graph) {
    require(graph.is_object() && !graph.empty(), "Starter graph is empty.");
    std::set<std::string> visiting, visited, outputs;
    for (const auto& id : starter.at("expected_outputs")) {
        const auto output = id.get<std::string>();
        require(graph.contains(output), "Intended starter output is absent.");
        const auto& schema = contracts.at(graph.at(output).at("class_type").get<std::string>());
        require(schema.at("output_node") == true, "Intended starter output is not an output node.");
        outputs.insert(output);
        inspectNode(graph, output, visiting, visited);
    }
    require(visited.size() == graph.size(), "Starter contains disconnected work that cannot reach the intended output.");
    for (const auto& [id, node] : graph.items()) {
        if (contracts.at(node.at("class_type").get<std::string>()).at("output_node") == true)
            require(outputs.contains(id), "Starter contains an undeclared provider output.");
    }
    for (const auto& model : starter.at("required_models")) {
        require(model.at("reuse_installed") == true, "Starter does not reuse its installed model identity.");
        for (const auto& binding : model.at("bindings")) {
            require(graph.at(binding.at("node_id").get<std::string>()).at("inputs").at(binding.at("input").get<std::string>()) == model.at("name"),
                "Starter graph model differs from its preparation manifest.");
        }
    }
}

void graphsMatchObservedContracts() {
    require(manifest.at("format") == "forge.comfyui.starters" && manifest.at("version") == 1U,
        "Starter manifest format or version changed without migration.");
    require(manifest.at("workflows").size() == 3U, "Image, text-to-video, and image-to-video starters are not all shipped.");
    for (const auto& starter : manifest.at("workflows")) {
        validateGraph(starter, graphFor(starter, "preview_file"));
        validateGraph(starter, graphFor(starter, "final_file"));
        require(starter.at("qualification").at("host_inference_verified") == false &&
            starter.at("qualification").at("operator_quality_accepted") == false,
            "Fixture qualification was incorrectly advertised as host generation or operator acceptance.");
    }
    for (const auto* field : {"managed_render_plan_required", "preview_approval_required", "recheck_live_contracts",
        "sealed_inputs_required", "resource_measurements_required_for_final", "do_not_apply_settings_to_unknown_graphs"})
        require(manifest.at("policy").at(field) == true, "Starter bypasses managed plan, approval, or resource policy.");
}

void videoDraftsDemonstrateTemporalContract() {
    for (const auto& starter : manifest.at("workflows")) {
        if (starter.at("media_type") != "video/mp4") continue;
        const auto preview = graphFor(starter, "preview_file"), final = graphFor(starter, "final_file");
        const auto& duration = starter.at("duration_contract");
        const auto latentId = duration.at("latent_node").get<std::string>();
        const auto fpsId = duration.at("fps_node").get<std::string>();
        for (const auto* stage : {"preview", "final"}) {
            const auto& graph = std::string_view{stage} == "preview" ? preview : final;
            const auto& dimensions = graph.at(latentId).at("inputs");
            const auto frames = dimensions.at("length").get<unsigned>();
            const auto fps = graph.at(fpsId).at("inputs").at("fps").get<double>();
            require(frames > 1U && (frames - 1U) % 4U == 0U, "Wan draft does not have motion frames on its actual temporal stride.");
            require(dimensions.at("width").get<unsigned>() % 32U == 0U && dimensions.at("height").get<unsigned>() % 32U == 0U,
                "Wan dimensions do not match the installed node step.");
            const auto seconds = frames / fps;
            require(seconds >= 4.9 && seconds <= 5.2, "Default Wan render no longer represents an approximately five-second video.");
            require(std::abs(seconds - starter.at("starting_settings").at(stage).at("duration_seconds").get<double>()) < 0.000001,
                "Duration metadata disagrees with frames and frame rate.");
            require(graph.at("12").at("inputs").at("format") == "mp4" && graph.at("12").at("inputs").at("codec") == "h264",
                "Default video delivery is not playable MP4/H264.");
        }
        require(preview.at("9").at("inputs").at("seed") == final.at("9").at("inputs").at("seed"),
            "Preview and final do not retain the starting creative seed.");
        const auto hasInput = starter.at("id") == "wan22_5b_i2v";
        require(preview.contains("1") == hasInput && final.contains("1") == hasInput,
            "Text-to-video and image-to-video input topology were conflated.");
        require(preview.at("7").at("inputs").contains("start_image") == hasInput && starter.at("input_bindings").empty() != hasInput,
            "Optional Wan start image does not agree with sealed input binding.");
    }
}

void parameterBindingsRemainTyped() {
    for (const auto& starter : manifest.at("workflows")) {
        for (const auto* stage : {"preview_file", "final_file"}) {
            auto graph = graphFor(starter, stage);
            for (const auto& [name, binding] : starter.at("parameters").items()) {
                const auto id = binding.at("node_id").get<std::string>(), input = binding.at("input").get<std::string>();
                require(graph.at(id).at("inputs").contains(input), "Model-facing typed parameter points to an absent input.");
                const auto type = binding.at("type").get<std::string>();
                if (name == "prompt") graph[id]["inputs"][input] = "Changed operator prompt";
                else if (name == "seed") graph[id]["inputs"][input] = 101U;
                require((type == "STRING" && graph[id]["inputs"][input].is_string()) ||
                    (type == "INT" && graph[id]["inputs"][input].is_number_integer()) ||
                    (type == "FLOAT" && graph[id]["inputs"][input].is_number()), "Typed parameter value violates its manifest contract.");
            }
            validateGraph(starter, Json::parse(graph.dump()));
        }
    }
}
}
}

int main(int argc, char** argv) {
    using namespace ForgeConductor::Tests;
    resources = argc > 1 ? Fs::path{argv[1]} : Fs::path{argv[0]}.parent_path() / "Resources" / "ComfyUI";
    const auto fixture = argc > 2 ? Fs::path{argv[2]} : Fs::path{__FILE__}.parent_path() / "Fixtures" / "comfy_starter_object_info.json";
    try {
        manifest = readJson(resources / "starter_manifest.json");
        const auto recorded = readJson(fixture);
        require(recorded.at("comfyui_contract_version") == "0.16.3", "Installed contract fixture version drifted.");
        contracts = recorded.at("nodes");
        TestRegistry tests{{"starter-graphs-installed-contracts", graphsMatchObservedContracts},
            {"starter-video-motion-duration-output", videoDraftsDemonstrateTemporalContract},
            {"starter-typed-parameter-roundtrip", parameterBindingsRemainTyped}};
        for (const auto& [name, run] : tests) { run(); std::cout << "PASS " << name << '\n'; }
    } catch (const std::exception& error) { std::cerr << "FAIL comfy starter workflows: " << error.what() << '\n'; return 1; }
    return 0;
}
