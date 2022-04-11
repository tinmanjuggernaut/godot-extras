#include "lod.h"
#include <cmath>

using namespace godot;

void MultiMeshLOD::_register_methods() {
    register_method("_process", &MultiMeshLOD::_process);
    register_method("_ready", &MultiMeshLOD::_ready);
    register_method("_enter_tree", &MultiMeshLOD::_enter_tree);
    register_method("_exit_tree", &MultiMeshLOD::_exit_tree);
    register_method("process_data", &MultiMeshLOD::process_data);
    // Inspector properties
    register_property<MultiMeshLOD, bool>("enabled", &MultiMeshLOD::set_enabled, &MultiMeshLOD::get_enabled, true);

    // Exposed methods
    register_method("update_lod_AABB", &MultiMeshLOD::update_lod_AABB);
    register_method("update_lod_multipliers_from_manager", &MultiMeshLOD::update_lod_multipliers_from_manager);
    // Whether to use distance multipliers from project settings
    register_property<MultiMeshLOD, bool>("affectedByDistanceMultipliers", &MultiMeshLOD::set_affected_by_distance, &MultiMeshLOD::get_affected_by_distance, true);

    // Screen percentage ratios (and if applicable)
    register_property<MultiMeshLOD, bool>("use_screen_percentage", &MultiMeshLOD::set_use_screen_percentage, &MultiMeshLOD::get_use_screen_percentage, true);

    // Vars for distance-based (in metres)
    // These will be set by the ratios if useScreenPercentage is true
    // Distance and ratio exposed names and variable names do not match to avoid massive compatability breakage with an older version of the addon.
    register_property<MultiMeshLOD, float>("minDist", &MultiMeshLOD::min_distance, 5.0f); 
    register_property<MultiMeshLOD, float>("maxDist", &MultiMeshLOD::max_distance, 80.0f); 

    register_property<MultiMeshLOD, float>("minRatio", &MultiMeshLOD::min_ratio, 2.0f);
    register_property<MultiMeshLOD, float>("maxRatio", &MultiMeshLOD::max_ratio, 5.0f);

    register_property<MultiMeshLOD, int64_t>("minCount", &MultiMeshLOD::min_count, 0); 
    register_property<MultiMeshLOD, int64_t>("maxCount", &MultiMeshLOD::max_count, -1); 

    register_property<MultiMeshLOD, float>("fade_speed", &MultiMeshLOD::fade_speed, 1.0f);
    register_property<MultiMeshLOD, float>("fade_exponent", &MultiMeshLOD::fade_exponent, 1.0f);

}

MultiMeshLOD::MultiMeshLOD() {
}

MultiMeshLOD::~MultiMeshLOD() {
    // add your cleanup here
}

void MultiMeshLOD::_init() {
}

void MultiMeshLOD::_exit_tree() {
    // Leave LOD manager's list.
    lc.unregister();

}

void MultiMeshLOD::_enter_tree() {
    // Ready and not registered? Probably re-entered the tree and need to re-regster.
    if (!lc.registered && lc.ready_finished) {
        lc.unregister();
        set_process(true);
    }
}

void MultiMeshLOD::_ready() {
    lc.setup(Object::cast_to<Spatial>(this));
    lc.lod_manager->debug_level_print(1, get_name() + String(": Initializing MultiMeshLOD."));

    if (get_class() != "MultiMeshInstance") {
        ERR_PRINT(get_name() + ": A MultiMeshLOD script is attached, but this is not a MultiMeshLOD!");
        lc.enabled = false;
        set_process(false);
        return;
    }

    multimesh = *get_multimesh();
    if (max_count < 0) {
        max_count = multimesh->get_instance_count();
    }
    target_count = max_count;

    update_lod_AABB();
    update_lod_multipliers_from_manager();
    lc.try_register();
    lc.ready_finished = true;
}

void MultiMeshLOD::process_data(Vector3 camera_location) {
    // Double check for this node being in the scene tree
    // (Otherwise you get errors when ending the thread)
    // Get the distance from the node to the camera
    Vector3 object_location;
    if (is_inside_tree()) {
        object_location = get_global_transform().origin;
    } else {
        return;
    }

    float distance = camera_location.distance_to(object_location);

    // Get our target value
    // ((max - current) / (max - min))^fadeExponent will give us the ratio of where we want to set our values
    // Multiply by how many we are interpolating then add the minimum
    float actual_max_distance = max_distance * global_distance_multiplier;
    float actual_min_distance = min_distance * global_distance_multiplier;
    target_count = int64_t(floor(pow(CLAMP((actual_max_distance - distance) / (actual_max_distance  - actual_min_distance), 0.0, 1.0), fade_exponent) * (max_count - min_count)));
    target_count += min_count;
}

void MultiMeshLOD::_process(float delta) {
    // Enter manager's list if not already done so (possibly due to timing issues upon game load)
    if (!lc.registered) {
        lc.try_register();
        set_process(false);
    }

    // Lerp visible instance count if needed
    int64_t instance_count = multimesh->get_visible_instance_count();
    if (instance_count != target_count) {
        /// Lerp
        // We normally floor the value, but in case of high FPS, our lerp might get stuck.
        // Let's get the equation result first. Then, if the difference is positive and above 0.001,
        // make sure we raise the count by at least 1.
        float next_value = CLAMP((instance_count + ((target_count - instance_count) * fade_speed * delta)), 0.1f, max_count);
        int next_instance_count = int64_t(floor(next_value));
        if (next_value - (float)instance_count > 0.001f && instance_count == next_instance_count) {
            next_instance_count++;
        }
        instance_count = next_instance_count;
        multimesh->set_visible_instance_count(instance_count);

        if ((instance_count == 0) && is_visible()) {
            hide();
        } else if ((instance_count > 0) && !is_visible()) {
            show();
        }
    }
}

// Update the distances based on the AABB
void MultiMeshLOD::update_lod_AABB() {
    if (lc.use_screen_percentage) {
        AABB object_AABB = get_transformed_aabb();

        if (object_AABB.has_no_area()) {
            ERR_PRINT(get_name() + ": Invalid AABB for this MultiMeshInstance!");
            return;
        }

        // Get the longest axis (conservative estimate of the object size vs screen)
        float longest_axis = object_AABB.get_longest_axis_size();

        // Get the distances at which we have the LOD ratios of the screen
        float tan_theta = lc.get_tan_theta();
        min_distance = ((longest_axis / (max_ratio / 100.0f)) / (2.0f * tan_theta));
        max_distance = ((longest_axis / (min_ratio / 100.0f)) / (2.0f * tan_theta));
    }
}

void MultiMeshLOD::update_lod_multipliers_from_manager() {
    if (lc.affected_by_distance_multipliers && lc.lod_manager) {
        global_distance_multiplier = lc.lod_manager->global_distance_multiplier;
    } else {
        global_distance_multiplier = 1.0f;
    }
}