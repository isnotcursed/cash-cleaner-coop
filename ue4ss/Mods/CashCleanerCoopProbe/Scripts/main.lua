local PREFIX = "[CashCleanerCoopProbe]"
local MAX_CLASSES = 5000
local scan_scheduled = false
local interaction_events = 0
local MAX_INTERACTION_EVENTS = 100
local target_classes = {
    "GameplayGameMode",
    "GameplayGameState",
    "GameplayPlayerController",
    "GameplayPlayerState",
    "PlayerCharacter",
    "InteractiveObject",
    "InteractiveObjectComponent",
    "InteractiveObjectSubsystem",
    "Inventory",
    "PickupCharacterComponent",
    "PickupObjectComponent",
    "PhysicsAttachCharacterComponent",
    "PhysicsAttachObjectComponent",
    "QuestSubsystem",
    "SaveSubsystem",
    "WorldStateSubsystem",
}
local function_keywords = {
    "attach", "complete", "destroy", "detach", "drop", "interact", "inventory",
    "load", "net", "pickup", "quest", "replic", "save", "spawn", "state",
}

local function log(message)
    print(string.format("%s %s\n", PREFIX, message))
end

local function is_game_class(full_name)
    local lower = string.lower(full_name)
    return string.find(lower, "/game/", 1, true) ~= nil
        or string.find(lower, "cashcleaner", 1, true) ~= nil
        or string.find(lower, "cleaner", 1, true) ~= nil
end

local function inventory_classes()
    local classes = {}
    local count = 0

    ForEachUObject(function(object)
        if count >= MAX_CLASSES or not object:IsValid() then
            return
        end

        local class = object:GetClass()
        if not class:IsValid() then
            return
        end

        local full_name = class:GetFullName()
        if is_game_class(full_name) and not classes[full_name] then
            classes[full_name] = true
            count = count + 1
        end
    end)

    local ordered = {}
    for class_name, _ in pairs(classes) do
        table.insert(ordered, class_name)
    end
    table.sort(ordered)

    log(string.format("inventory begin: %d unique classes", #ordered))
    for _, class_name in ipairs(ordered) do
        log("class " .. class_name)
    end
    log("inventory end")
end

local function is_relevant_function(full_name)
    local lower = string.lower(full_name)
    for _, keyword in ipairs(function_keywords) do
        if string.find(lower, keyword, 1, true) then
            return true
        end
    end
    return false
end

local parameter_probe_functions = {
    ["Function /Script/CashCleanerSim.PickupCharacterComponent:Pickup"] = true,
    ["Function /Script/CashCleanerSim.PickupCharacterComponent:Grab"] = true,
    ["Function /Script/CashCleanerSim.PickupCharacterComponent:Drop"] = true,
    ["Function /Script/CashCleanerSim.PickupCharacterComponent:Throw"] = true,
    ["Function /Script/CashCleanerSim.PickupObjectComponent:SetIsPickedUp"] = true,
}

local function safe_property(object, name)
    local ok, value = pcall(function()
        return object:GetPropertyValue(name)
    end)
    if not ok then
        return "unavailable"
    end
    return tostring(value)
end

local function inventory_targets()
    log("target inventory begin")
    for _, short_name in ipairs(target_classes) do
        local class = StaticFindObject("/Script/CashCleanerSim." .. short_name)
        if not class:IsValid() then
            log("target missing " .. short_name)
        else
            local cdo = class:GetCDO()
            log(string.format(
                "target %s defaults bReplicates=%s bAlwaysRelevant=%s NetUpdateFrequency=%s NetDormancy=%s",
                short_name,
                safe_property(cdo, "bReplicates"),
                safe_property(cdo, "bAlwaysRelevant"),
                safe_property(cdo, "NetUpdateFrequency"),
                safe_property(cdo, "NetDormancy")
            ))

            class:ForEachFunction(function(fn)
                local full_name = fn:GetFullName()
                if is_relevant_function(full_name) then
                    log(string.format("function flags=0x%X %s", fn:GetFunctionFlags(), full_name))
                end
                if parameter_probe_functions[full_name] then
                    fn:ForEachProperty(function(property)
                        log(string.format("parameter %s class=%s",
                            property:GetFName():ToString(),
                            property:GetClass():GetFName():ToString()))
                    end)
                end
            end)
        end
    end
    log("target inventory end")
end

local function inventory_live_player()
    local player = FindFirstOf("PlayerCharacter")
    if not player:IsValid() then
        log("live player unavailable")
        return
    end
    log("live player object " .. player:GetFullName())
    log("live player class " .. player:GetClass():GetFullName())
    for _, name in ipairs({
        "Mesh", "CharacterMesh", "FirstPersonMesh", "BodyMesh",
        "Camera", "FirstPersonCamera", "CameraComponent",
    }) do
        local ok, full_name = pcall(function()
            local value = player:GetPropertyValue(name)
            if not value or not value:IsValid() then
                return nil
            end
            return value:GetFullName()
        end)
        if ok and full_name then
            log(string.format("live player property %s=%s", name, full_name))
        end
    end
end

local function schedule_inventory()
    if scan_scheduled then
        return
    end
    scan_scheduled = true

    ExecuteWithDelay(5000, function()
        ExecuteInGameThread(function()
            inventory_classes()
            inventory_targets()
            inventory_live_player()
            scan_scheduled = false
        end)
    end)
end

local function describe_hook_value(value)
    local ok, description = pcall(function()
        local object = value:get()
        if object and object:IsValid() then
            return object:GetFullName()
        end
        return tostring(object)
    end)
    return ok and description or "unavailable"
end

local function trace_interaction(name, self, ...)
    if interaction_events >= MAX_INTERACTION_EVENTS then
        return
    end
    interaction_events = interaction_events + 1
    local parts = {name, "self=" .. describe_hook_value(self)}
    for index, value in ipairs({...}) do
        table.insert(parts, string.format("arg%d=%s", index, describe_hook_value(value)))
    end
    log("interaction " .. table.concat(parts, " "))
end

for _, target in ipairs({
    "/Script/CashCleanerSim.PickupCharacterComponent:Pickup",
    "/Script/CashCleanerSim.PickupCharacterComponent:Grab",
    "/Script/CashCleanerSim.PickupCharacterComponent:Drop",
    "/Script/CashCleanerSim.PickupCharacterComponent:Throw",
    "/Script/CashCleanerSim.PickupObjectComponent:SetIsPickedUp",
}) do
    local name = target
    RegisterHook(target, function(self, ...)
        trace_interaction(name, self, ...)
    end)
end

RegisterLoadMapPostHook(function()
    log("map loaded; scheduling class inventory")
    schedule_inventory()
end)

ExecuteWithDelay(10000, function()
    ExecuteInGameThread(schedule_inventory)
end)

log("loaded")
