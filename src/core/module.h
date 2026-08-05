// Compiled Phonoscope module.
//
// The dashboard compiles module YAML to JSON and serves it from
// `/api/phonoscope/modules/{id}/{version}/compiled`. Both engines consume that
// same artefact, so nothing in `nova-visualiser-modules/` changes and no new
// module format exists. This is a decode of that JSON, matching
// `PhonoscopeModule` in `PhonoscopeModels.swift`.
#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/json.h"
#include "core/vec.h"

namespace nova {

struct ModuleSetting {
  std::string id;
  std::string label;
  std::string description;
  std::string control;
  double min = 0;
  double max = 1;
  double step = 0.01;
  double defaultValue = 0;
  std::string section;
  std::string updateMode;
  std::string curveType;
  double curveExponent = 1;
  // Declarative control metadata from the module (spec section 3). The engine
  // uses it to decide which settings can be applied to live entities without a
  // structural rebuild.
  std::vector<std::string> affects;
};

struct PaletteSlotDeclaration {
  std::string id;
  std::string label;
  Vec4 defaultRgb{0, 0, 0, 1};
};

struct Boundary {
  std::string mode = "bounce";
  double restitution = 0.82;
  std::string then;
  std::string effect;
};

struct Resources {
  int maxParticles = 4096;
  int maxInteractiveFieldEntities = 1024;
  int maxRenderBatches = 16;
};

class Module {
 public:
  static std::optional<Module> decode(const json::Value& value);
  static std::optional<Module> parse(std::string_view json);

  const std::string& id() const { return id_; }
  const std::string& version() const { return version_; }
  const std::string& name() const { return name_; }
  const std::string& dimension() const { return dimension_; }
  bool is3D() const { return dimension_ == "3d"; }

  Vec3 minimum() const { return minimum_; }
  Vec3 maximum() const { return maximum_; }

  const Boundary& boundary() const { return boundary_; }
  const Resources& resources() const { return resources_; }
  const std::vector<ModuleSetting>& settings() const { return settings_; }
  const std::vector<PaletteSlotDeclaration>& paletteSlots() const { return paletteSlots_; }
  const std::vector<json::Value>& scene() const { return scene_; }

  const json::Value* templateFor(const std::string& name) const {
    auto it = templates_.find(name);
    return it == templates_.end() ? nullptr : &it->second;
  }

  // Stable identity used for cache keys, seeding, and change detection.
  std::string key() const { return id_ + "@" + version_; }

 private:
  std::string id_;
  std::string version_;
  std::string name_;
  std::string description_;
  std::string dimension_ = "2d";
  Vec3 minimum_{-1, -1, 0};
  Vec3 maximum_{1, 1, 0};
  Boundary boundary_;
  Resources resources_;
  std::vector<ModuleSetting> settings_;
  std::vector<PaletteSlotDeclaration> paletteSlots_;
  std::unordered_map<std::string, json::Value> templates_;
  std::vector<json::Value> scene_;
};

// Swift's `String.hashValue` is randomly seeded per process, so it cannot be
// used for anything that has to agree between engines or between runs. The
// module seed is therefore an explicit FNV-1a over the module id.
uint64_t moduleSeed(const std::string& moduleId);

}  // namespace nova
