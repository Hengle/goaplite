#include "goap.h"
#include <stdio.h>
#include "cJSON.h"
#include "log/log.h"

#define ITERATE_ARRAY(array) goap_action_t *it = da_begin(array), *end = da_end(array); it != end; ++it
#define ITERATE_ARRAYPTR(array) goap_action_t **it = da_begin(array), **end = da_end(array); it != end; ++it

/** a node used for graph searching */
typedef struct {
    goap_actionlist_t parents;
    /** current world state at this node (sum of all parents world states, basically) */
    goap_worldstate_t worldState;
    /** total cost of this node so far */
    uint32_t cost;
} node_t;
DA_TYPEDEF(node_t, nodelist_t)
DA_TYPEDEF(goap_action_t*, actionlistptr_t)
/** list of action lists */
DA_TYPEDEF(goap_actionlist_t, actionlist2_t)

/** returns true if the given action can be executed in the current world state */
static bool can_perform_action(goap_action_t action, goap_worldstate_t world){
    return goap_worldstate_compare(world, action.preConditions);
}

/** updates the specified world state by applying the post conditions of the specified action. works "in place" on world */
static void execute_action(goap_action_t action, goap_worldstate_t *world){
    map_iter_t iter = map_iter();
    const char *key = NULL;

    // let's pretend we executed the action, so apply our post-conditions to the backup world
    while ((key = map_next(&action.postConditions, &iter))){
        bool *postResult = map_get(&action.postConditions, key);
        map_set(world, key, *postResult);
    }
}

/** checks if the given action list contains an entry with the name of "name" */
static bool contains_name(char *name, goap_actionlist_t history){
    for (ITERATE_ARRAY(history)){
        if (strcmp(it->name, name) == 0){
            return true;
        }
    }
    return false;
}

/** returns the list of actions that can be executed from this node given it's parenets and current state */
static goap_actionlist_t find_executable_actions(node_t node, goap_actionlist_t actions){
    goap_actionlist_t neighbours = {0};
    for (ITERATE_ARRAY(actions)){
        // exclude actions that we cannot execute or that are our parents
        if (can_perform_action(*it, node.worldState) && !contains_name(it->name, node.parents)){
            da_add(neighbours, *it);
        }
    }
    return neighbours;
}

/** clones the specified map to a new map */
static goap_worldstate_t map_clone(goap_worldstate_t oldMap){
    goap_worldstate_t newMap = {0};
    map_iter_t iter = map_iter();
    const char *key = NULL;
    while ((key = map_next(&oldMap, &iter))){
        map_set(&newMap, key, map_get(&oldMap, key));
    }
    return newMap;
}

/** clones the specified list to a new list */
static goap_actionlist_t list_clone(goap_actionlist_t oldList){
    goap_actionlist_t newList = {0};
    for (ITERATE_ARRAY(oldList)){
        da_add(newList, *it);
    }
    return newList;
}

goap_actionlist_t goap_planner_plan(goap_worldstate_t currentWorld, goap_worldstate_t goal, goap_actionlist_t allActions){
    log_trace("GOAP planner working with %zu actions", da_count(allActions));
    goap_actionlist_t plan = {0};

    // check if we're already at the goal for some reason
    if (goap_worldstate_compare(currentWorld, goal)){
        log_warn("Goal state is already satisfied, no planning required");
        return plan;
    }

    // use a depth first search
    nodelist_t stack = {0};
    actionlistptr_t garbage = {0};

    // add our current state to the stack
    node_t initial = {0};
    initial.worldState = currentWorld;
    da_add(stack, initial);

    while (da_count(stack) > 0){
        printf("\nStack has %zu elements\n", da_count(stack));
        // pop the last element off the stack, we can copy it since we're throwing it away
        node_t node = da_pop(stack);

        // (just debug stuff)
        printf("Visiting node with %zu parents\n", da_count(node.parents));
        if (da_count(node.parents) > 0){
            printf("Parents are:\n");
            goap_actionlist_dump(node.parents);
        }
        printf("World state of this node is:\n");
        goap_worldstate_dump(node.worldState);

       // let's see what actions we can execute in the current world state of the node
       goap_actionlist_t neighbours = find_executable_actions(node, allActions);
       printf("List of actions we can perform from this state:\n");
       goap_actionlist_dump(neighbours);

       // iterate through each action and put a new node on the search list
       for (ITERATE_ARRAY(neighbours)){
           // let's pretend we executed the action and see what our new world state looks like
           // we first have to duplicate the node so we don't cause heap use after frees (somehow?)
           goap_worldstate_t newWorld = map_clone(node.worldState);
           execute_action(*it, &newWorld);
           printf("After performing %s, new world state is:\n", it->name);
           goap_worldstate_dump(newWorld);

           // check if goal state reached
           if (goap_worldstate_compare(newWorld, goal)){
               // TODO trace back, calc cost and add to list of possible paths
               printf("Reached goal!\n");
           } else {
               // clone the list as well, for the same reasons as the map
               goap_actionlist_t parentsClone = list_clone(node.parents);
               da_add(parentsClone, *it);

               // make a new node with the updated parents and new world state
               node_t newNode ={0};
               newNode.parents = parentsClone;
               newNode.worldState = newWorld;
               da_add(stack, newNode);
               printf("Added new node with %zu parents to stack\n", da_count(newNode.parents));
           }
       }

       // free the node's contents now that we no longer need it
       map_deinit(&node.worldState);
       da_free(node.parents);
       da_free(neighbours);
    }

    // TODO select best path with least cost

    // free any extraneous actions we allocated during the search
    for (ITERATE_ARRAYPTR(garbage)){
        printf("Freeing allocated node: %s\n", (*it)->name);
        free(*it);
    }

    da_free(garbage);
    da_free(stack);
    return plan;
}

goap_actionlist_t goap_parse_json(char *str, size_t length){
    log_trace("Using cJSON v%s", cJSON_Version());
    cJSON *json = cJSON_ParseWithLength(str, length);
    goap_actionlist_t out = {0};
    if (json == NULL){
        log_error("Failed to parse JSON document: token %s\n", cJSON_GetErrorPtr());
        goto finish;
    }

    cJSON *actions = cJSON_GetObjectItem(json, "actions");
    if (!cJSON_IsArray(actions)){
        log_error("Invalid JSON document: actions array is not an array, or doesn't exist");
        goto finish;
    }

    cJSON *action = NULL;
    cJSON_ArrayForEach(action, actions){
        cJSON *name = cJSON_GetObjectItem(action, "name");
        cJSON *cost = cJSON_GetObjectItem(action, "cost");
        cJSON *preConditions = cJSON_GetObjectItem(action, "preConditions");
        cJSON *postConditions = cJSON_GetObjectItem(action, "postConditions");
        char *dump = cJSON_Print(action);

        // validate our parsed data
        if (!cJSON_IsString(name)){
            log_error("Invalid JSON object: action name is not a string or doesn't exist\n%s", dump);
            free(dump);
            break;
        } else if (!cJSON_IsNumber(cost)){
            log_error("Invalid JSON object: action cost is not a number or doesn't exist\n%s", dump);
            free(dump);
            break;
        } else if (!cJSON_IsObject(preConditions)){
            log_error("Invalid JSON object: action preConditions is not an object or doesn't exist\n%s", dump);
            free(dump);
            break;
        } else if (!cJSON_IsObject(postConditions)){
            log_error("Invalid JSON object: action postConditions is not an object or doesn't exist\n%s", dump);
            free(dump);
            break;
        } else {
            log_trace("Verification passed for config object");
            free(dump);
        }
        // additional requirements that are not checked here:
        // - each action MUST have a unique string name

        // now that we've got a valid document, serialise the JSON into a GOAP action
        goap_action_t parsedAction = {0};
        // because cJSON_Delete() apparently also deletes all the strings we have to strdup() the name
        parsedAction.name = strdup(name->valuestring);
        parsedAction.cost = cost->valueint;
        map_init(&parsedAction.preConditions);
        map_init(&parsedAction.postConditions);

        // apparently this works for objects to
        cJSON *preItem = NULL;
        cJSON_ArrayForEach(preItem, preConditions){
            map_set(&parsedAction.preConditions, preItem->string, preItem->valueint);
        }
        cJSON *postItem = NULL;
        cJSON_ArrayForEach(postItem, postConditions){
            map_set(&parsedAction.postConditions, postItem->string, postItem->valueint);
        }

        // TODO we'd also need to set the function pointers based on name here or later somewhere else
        da_add(out, parsedAction);
    }

    finish:
    cJSON_Delete(json);
    return out;
}

void goap_actionlist_free(goap_actionlist_t *list){
    for (size_t i = 0; i < da_count(*list); i++){
        goap_action_t action = da_get(*list, i);
        free(action.name);
        map_deinit(&action.preConditions);
        map_deinit(&action.postConditions);
        // action itself is stack allocated (or something like that) so no need to free it
    }
    da_free(*list);
}

void goap_actionlist_dump(goap_actionlist_t list){
    //printf("goap_actionlist_dump() %zu entries:\n", da_count(list));
    if (da_count(list) == 0){
        printf("\t(empty action list)\n");
        return;
    }

    for (size_t i = 0; i < da_count(list); i++){
        printf("\t%zu. %s\n", i + 1, da_get(list, i).name);
    }
}

void goap_worldstate_dump(goap_worldstate_t world){
    //printf("goap_worldstate_dump() %d entries:\n", world.base.nnodes);
    map_iter_t iter = map_iter();
    const char *key = NULL;
    uint32_t count = 0;
    while ((key = map_next(&world, &iter))) {
        printf("\t%s: %s\n", key, *map_get(&world, key) ? "true" : "false");
        count++;
    }

    if (count == 0){
        printf("\t(empty world state)\n");
    }
}

bool goap_worldstate_compare_strict(goap_worldstate_t a, goap_worldstate_t b){
    // TODO this function may be broke, should rewrite
    // must have same number of keys
    if (a.base.nnodes != b.base.nnodes) return false;
    map_iter_t iter = map_iter();

    const char *key = NULL;
    while ((key = map_next(&a, &iter))) {
        bool *aValue = map_get(&a, key);
        bool *bValue = map_get(&b, key);

        // one of the maps does not contain the key
        if (aValue == NULL || bValue == NULL || *aValue != *bValue){
            return false;
        }
    }
    return true;
}

bool goap_worldstate_compare(goap_worldstate_t currentState, goap_worldstate_t goal){
    map_iter_t iter = map_iter();
    const char *key = NULL;

    while ((key = map_next(&goal, &iter))){
        bool *curVal = map_get(&currentState, key);
        bool *targetVal = map_get(&goal, key);

        if (curVal == NULL || targetVal == NULL || *curVal != *targetVal){
            return false;
        }
    }
    return true;
}