#include "unity_path.hpp"

#include "log.hpp"
#include "mem.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace gyro4all::unity {

bool holds_object(Runtime& runtime, int type, void* value_class) {
    if (type == kTypeClass || type == kTypeObject || type == kTypeArray) return true;
    return type == kTypeGeneric && value_class && !runtime.class_is_value_type(value_class);
}

namespace {

enum class Container { None, Array, List, Dictionary, LinkedList };

Container container_kind(Runtime& runtime, void* klass) {
    if (!klass) return Container::None;
    const std::string_view name = runtime.class_name(klass), space = runtime.class_namespace(klass);
    if (name.ends_with("[]")) return Container::Array;
    if (space != "System.Collections.Generic") return Container::None;
    if (name == "LinkedList`1") return Container::LinkedList;
    return name == "List`1" ? Container::List : name == "Dictionary`2" ? Container::Dictionary : Container::None;
}

// Newer class libraries name them with an underscore.
void* either_field(Runtime& runtime, void* klass, const char* first, const char* second) {
    void* field = runtime.find_field(klass, first);
    return field ? field : runtime.find_field(klass, second);
}

}  // namespace

bool is_container(Runtime& runtime, void* klass) { return container_kind(runtime, klass) != Container::None; }

std::vector<void*> container_objects(Runtime& runtime, void* container, size_t max) {
    std::vector<void*> out;
    if (!container) return out;
    void* klass = runtime.object_class(container);
    const Container kind = container_kind(runtime, klass);
    if (kind == Container::None) return out;
    if (kind == Container::LinkedList) {
        // A ring of nodes from the first (Firewatch's camera mode stack, newest first). Old Mono names its fields first,
        // forward and count; the .NET class library head, next and count. Both rings close on the first node.
        void* first = either_field(runtime, klass, "head", "first");
        void* size = either_field(runtime, klass, "count", "_count");
        int stored = 0;
        if (!first || !size || !mem::rd(reinterpret_cast<uintptr_t>(container) + runtime.field_offset(size), stored) || stored <= 0) return out;
        const uintptr_t head = mem::rdp(reinterpret_cast<uintptr_t>(container) + runtime.field_offset(first));
        void* node_class = head ? runtime.object_class(reinterpret_cast<void*>(head)) : nullptr;
        void* item = node_class ? runtime.find_field(node_class, "item") : nullptr;
        void* next = node_class ? either_field(runtime, node_class, "next", "forward") : nullptr;
        if (!item || !next || !holds_object(runtime, runtime.field_type(item), runtime.field_class(item))) return out;
        uintptr_t node = head;
        for (int i = 0; i < stored && node && out.size() < max; ++i) {
            const uintptr_t object = mem::rdp(node + runtime.field_offset(item));
            if (object) out.push_back(reinterpret_cast<void*>(object));
            node = mem::rdp(node + runtime.field_offset(next));
            if (node == head) break;
        }
        return out;
    }
    void* array = container;
    size_t count = 0;
    if (kind == Container::List) {
        void* items = either_field(runtime, klass, "_items", "items");
        void* size = either_field(runtime, klass, "_size", "size");
        int stored = 0;
        if (!items || !size || !mem::rd(reinterpret_cast<uintptr_t>(container) + runtime.field_offset(size), stored) || stored <= 0) return out;
        array = reinterpret_cast<void*>(mem::rdp(reinterpret_cast<uintptr_t>(container) + runtime.field_offset(items)));
        count = size_t(stored);
    } else if (kind == Container::Dictionary) {
        void* entries = either_field(runtime, klass, "_entries", "entries");
        if (!entries) return out;
        array = reinterpret_cast<void*>(mem::rdp(reinterpret_cast<uintptr_t>(container) + runtime.field_offset(entries)));
    }
    if (!array) return out;
    const size_t length = runtime.array_length(array);
    count = kind == Container::List ? std::min(count, length) : length;
    void* array_class = runtime.object_class(array);
    void* element = runtime.element_class(array_class);
    if (!element) return out;
    size_t stride = sizeof(void*), offset = 0;
    if (kind == Container::Dictionary) {
        void* value = runtime.find_field(element, "value");
        stride = runtime.element_size(element);
        if (!value || !stride || !holds_object(runtime, runtime.field_type(value), runtime.field_class(value))) return out;
        offset = runtime.field_offset(value) - kObjectHeader;
    } else if (runtime.class_is_value_type(element)) {
        return out;
    }
    for (size_t i = 0; i < count && out.size() < max; ++i) {
        void* address = runtime.array_element(array, i, stride);
        const uintptr_t object = address ? mem::rdp(reinterpret_cast<uintptr_t>(address) + offset) : 0;
        if (object) out.push_back(reinterpret_cast<void*>(object));
    }
    return out;
}

ManagedPath::ManagedPath(std::vector<std::string> parts) : parts_(std::move(parts)), steps_(parts_.size()) {}

void ManagedPath::complain(const std::string& text) {
    if (complained_ == text) return;
    complained_ = text;
    log("unity: profile path: %s", text.c_str());
}

bool ManagedPath::resolve(Runtime& runtime, void* root, FieldValue& out) {
    if (!root || parts_.empty() || !runtime.has_reflection()) return false;
    uintptr_t base = reinterpret_cast<uintptr_t>(root);  // what field offsets are added to (they count the object header)
    void* klass = runtime.object_class(root);
    bool inline_value = false, copy = false;
    for (size_t i = 0; i < parts_.size(); ++i) {
        const std::string& part = parts_[i];
        const bool last = i + 1 == parts_.size();
        Step& step = steps_[i];
        const bool method = part.ends_with("(@)");
        const bool call = !method && part.ends_with("()");  // a method with no arguments, read like a getter
        const size_t bracket = !method && !call && part.ends_with("]") ? part.find('[') : std::string::npos;
        const std::string field_name = bracket == std::string::npos ? part : part.substr(0, bracket);
        if (step.klass != klass) {
            step = {};
            if (method) {
                const std::string name = part.substr(0, part.size() - 3);
                step.member = runtime.find_method(klass, name.c_str(), 1);
                if (!step.member) {
                    complain("method " + name + " not found on " + runtime.class_full_name(klass));
                    return false;
                }
            } else if (call) {
                const std::string name = part.substr(0, part.size() - 2);
                step.member = runtime.find_method(klass, name.c_str(), 0);
                step.getter = step.member != nullptr;
                if (!step.member) {
                    complain("method " + name + "() not found on " + runtime.class_full_name(klass));
                    return false;
                }
            } else {
                step.member = runtime.find_field(klass, field_name.c_str());
                if (!step.member && bracket == std::string::npos) {
                    step.member = runtime.find_method(klass, ("get_" + field_name).c_str(), 0);
                    step.getter = step.member != nullptr;
                }
                if (!step.member) {
                    complain("field " + field_name + " not found on " + runtime.class_full_name(klass));
                    return false;
                }
            }
            if (!method && !step.getter) {
                if (runtime.field_is_static(step.member)) {
                    complain("field " + field_name + " of " + runtime.class_full_name(klass) + " is static (not supported)");
                    return false;
                }
                step.offset = runtime.field_offset(step.member);
                step.type = runtime.field_type(step.member);
                step.value_class = runtime.field_class(step.member);
            }
            step.klass = klass;
        }
        if (method) {
            if (last || inline_value) {
                complain("method " + part + (last ? " can't end a path" : " called on a value type field"));
                return false;
            }
            void* arguments[] = {root};
            void* result = runtime.invoke(step.member, reinterpret_cast<void*>(base), arguments);
            if (!result) return false;
            base = reinterpret_cast<uintptr_t>(result);
            klass = runtime.object_class(result);
            if (runtime.class_is_value_type(klass)) copy = true;
            continue;
        }
        if (step.getter) {
            if (inline_value) {
                complain("property " + part + " read on a value type field (not supported)");
                return false;
            }
            void* result = runtime.invoke(step.member, reinterpret_cast<void*>(base));
            if (!result) return false;
            if (last) {
                out = {};
                out.copy = out.called = true;
                out.returned = result;
                return true;
            }
            base = reinterpret_cast<uintptr_t>(result);  // a boxed value type keeps its header: field offsets still fit
            klass = runtime.object_class(result);
            if (runtime.class_is_value_type(klass)) copy = true;
            continue;
        }
        const uintptr_t address = base + step.offset;
        if (bracket != std::string::npos) {
            if (last) {
                complain("a container part can't end a path");
                return false;
            }
            const std::string wanted = part.substr(bracket + 1, part.size() - bracket - 2);
            void* found = nullptr;
            for (void* object : container_objects(runtime, reinterpret_cast<void*>(mem::rdp(address)), 64)) {
                if (wanted == runtime.class_name(runtime.object_class(object))) {
                    found = object;
                    break;
                }
            }
            if (!found) return false;
            base = reinterpret_cast<uintptr_t>(found);
            klass = runtime.object_class(found);
            inline_value = false;
            continue;
        }
        if (last) {
            out = {address, step.type, step.value_class, copy};
            return true;
        }
        if (step.type == kTypeValueType) {
            base = address - kObjectHeader;
            klass = step.value_class;
            inline_value = true;
        } else if (holds_object(runtime, step.type, step.value_class)) {
            base = mem::rdp(address);
            if (!base) return false;
            klass = runtime.object_class(reinterpret_cast<void*>(base));
            inline_value = false;
        } else {
            complain("field " + part + " is neither an object nor a value type: the path can't go through it");
            return false;
        }
    }
    return false;
}

namespace {
constexpr int kTypeBoolean = 0x02, kTypeI4 = 0x08, kTypeR8 = 0x0d;
}

bool ManagedPath::read_bool(Runtime& runtime, void* root, bool& value) {
    FieldValue v;
    if (!resolve(runtime, root, v)) return false;
    uint8_t byte = 0;
    if (v.called) {
        if (!runtime.returned(v.returned, byte)) return false;
    } else if (v.type != kTypeBoolean || !mem::rd(v.address, byte)) {
        if (v.type != kTypeBoolean) complain("a Look= condition must end at a bool");
        return false;
    }
    value = byte != 0;
    return true;
}

bool ManagedPath::read_number(Runtime& runtime, void* root, double& value) {
    FieldValue v;
    if (!resolve(runtime, root, v)) return false;
    if (v.called) {
        float f = 0;
        if (!runtime.returned(v.returned, f)) return false;
        value = f;
    } else if (v.type == kTypeR4) {
        float f = 0;
        if (!mem::rd(v.address, f)) return false;
        value = f;
    } else if (v.type == kTypeR8) {
        if (!mem::rd(v.address, value)) return false;
    } else if (v.type == kTypeI4) {
        int32_t i = 0;
        if (!mem::rd(v.address, i)) return false;
        value = i;
    } else {
        complain("a number path must end at a float, double or int");
        return false;
    }
    return std::isfinite(value);
}

void* ManagedPath::read_object(Runtime& runtime, void* root) {
    FieldValue v;
    if (!resolve(runtime, root, v)) return nullptr;
    if (v.called) return runtime.class_is_value_type(runtime.object_class(v.returned)) ? nullptr : v.returned;
    if (!holds_object(runtime, v.type, v.klass)) {
        complain("an object path must end at a field holding an object");
        return nullptr;
    }
    return reinterpret_cast<void*>(mem::rdp(v.address));
}

}  // namespace gyro4all::unity
