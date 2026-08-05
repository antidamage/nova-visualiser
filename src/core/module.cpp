#include "core/module.h"

#include <algorithm>

namespace nova {
namespace {

double numberAt(const json::Value* value, size_t index, double fallback) {
  if (value == nullptr) return fallback;
  const json::Array* items = value->array();
  if (items == nullptr || index >= items->size()) return fallback;
  return (*items)[index].numberOr(fallback);
}

std::string stringField(const json::Value& object, std::string_view key,
                        std::string_view fallback = "") {
  const json::Value* value = object.find(key);
  return value != nullptr ? value->stringOr(fallback) : std::string(fallback);
}

double numberField(const json::Value& object, std::string_view key, double fallback) {
  const json::Value* value = object.find(key);
  return value != nullptr ? value->numberOr(fallback) : fallback;
}

}  // namespace

uint64_t moduleSeed(const std::string& moduleId) {
  uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : moduleId) {
    hash = (hash ^ static_cast<uint64_t>(c)) * 1099511628211ULL;
  }
  return hash;
}

std::optional<Module> Module::parse(std::string_view text) {
  auto value = json::Value::parse(text);
  if (!value) return std::nullopt;
  // The compiled endpoint may wrap the module in `{ "module": ... }`.
  if (const json::Value* wrapped = value->find("module")) return decode(*wrapped);
  return decode(*value);
}

std::optional<Module> Module::decode(const json::Value& value) {
  if (value.object() == nullptr) return std::nullopt;

  Module module;
  module.id_ = stringField(value, "id");
  module.version_ = stringField(value, "version", "1.0.0");
  module.name_ = stringField(value, "name", module.id_);
  module.description_ = stringField(value, "description");
  module.dimension_ = stringField(value, "dimension", "2d");
  if (module.id_.empty()) return std::nullopt;

  const bool is3D = module.dimension_ == "3d";
  if (const json::Value* bounds = value.find("bounds")) {
    const json::Value* minimum = bounds->find("min");
    const json::Value* maximum = bounds->find("max");
    module.minimum_ = Vec3{static_cast<float>(numberAt(minimum, 0, -1)),
                           static_cast<float>(numberAt(minimum, 1, -1)),
                           static_cast<float>(numberAt(minimum, 2, is3D ? -1 : 0))};
    module.maximum_ = Vec3{static_cast<float>(numberAt(maximum, 0, 1)),
                           static_cast<float>(numberAt(maximum, 1, 1)),
                           static_cast<float>(numberAt(maximum, 2, is3D ? 1 : 0))};
  } else {
    module.minimum_ = Vec3{-1, -1, is3D ? -1.0f : 0.0f};
    module.maximum_ = Vec3{1, 1, is3D ? 1.0f : 0.0f};
  }

  if (const json::Value* boundary = value.find("boundary")) {
    if (auto text = boundary->string()) {
      module.boundary_.mode = *text;
    } else if (boundary->object() != nullptr) {
      module.boundary_.mode = stringField(*boundary, "mode", "bounce");
      module.boundary_.restitution = numberField(*boundary, "restitution", 0.82);
      module.boundary_.then = stringField(*boundary, "then");
      module.boundary_.effect = stringField(*boundary, "effect");
    }
  }

  if (const json::Value* resources = value.find("resources")) {
    module.resources_.maxParticles =
        static_cast<int>(numberField(*resources, "maxParticles", 4096));
    module.resources_.maxInteractiveFieldEntities =
        static_cast<int>(numberField(*resources, "maxInteractiveFieldEntities", 1024));
    module.resources_.maxRenderBatches =
        static_cast<int>(numberField(*resources, "maxRenderBatches", 16));
  }
  // Spec section 2 ceilings. A module may request less, never more.
  module.resources_.maxParticles = clampValue(module.resources_.maxParticles, 1, 65536);
  module.resources_.maxInteractiveFieldEntities =
      clampValue(module.resources_.maxInteractiveFieldEntities, 1, 16384);
  module.resources_.maxRenderBatches = clampValue(module.resources_.maxRenderBatches, 1, 64);

  if (const json::Value* settings = value.find("settings")) {
    if (const json::Array* items = settings->array()) {
      for (const json::Value& item : *items) {
        ModuleSetting setting;
        setting.id = stringField(item, "id");
        if (setting.id.empty()) continue;
        setting.label = stringField(item, "label", setting.id);
        setting.description = stringField(item, "description");
        setting.control = stringField(item, "control", "slider");
        setting.min = numberField(item, "min", 0);
        setting.max = numberField(item, "max", 1);
        setting.step = numberField(item, "step", 0.01);
        setting.defaultValue = numberField(item, "default", setting.min);
        setting.section = stringField(item, "section");
        setting.updateMode = stringField(item, "updateMode", "smooth");
        if (const json::Value* curve = item.find("curve")) {
          setting.curveType = stringField(*curve, "type", "linear");
          setting.curveExponent = numberField(*curve, "exponent", 1);
        }
        if (const json::Value* affects = item.find("affects")) {
          setting.affects = affects->stringArray();
        }
        module.settings_.push_back(std::move(setting));
      }
    }
  }

  if (const json::Value* slots = value.find("paletteSlots")) {
    if (const json::Array* items = slots->array()) {
      for (const json::Value& item : *items) {
        PaletteSlotDeclaration slot;
        slot.id = stringField(item, "id");
        if (slot.id.empty()) continue;
        slot.label = stringField(item, "label", slot.id);
        const json::Value* rgb = item.find("defaultRgb");
        slot.defaultRgb = Vec4{static_cast<float>(numberAt(rgb, 0, 0) / 255.0),
                               static_cast<float>(numberAt(rgb, 1, 0) / 255.0),
                               static_cast<float>(numberAt(rgb, 2, 0) / 255.0), 1.0f};
        module.paletteSlots_.push_back(std::move(slot));
      }
    }
  }

  if (const json::Value* templates = value.find("templates")) {
    if (const json::Object* items = templates->object()) {
      for (const auto& [key, item] : *items) module.templates_[key] = item;
    }
  }

  if (const json::Value* scene = value.find("scene")) {
    if (const json::Array* items = scene->array()) {
      module.scene_.assign(items->begin(), items->end());
    }
  }

  return module;
}

}  // namespace nova
