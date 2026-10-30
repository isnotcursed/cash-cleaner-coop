local PREFIX = "[CashCleanerCoopProbe]"
local MAX_CLASSES = 5000
local scan_scheduled = false

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

local function schedule_inventory()
    if scan_scheduled then
        return
    end
    scan_scheduled = true

    ExecuteWithDelay(5000, function()
        ExecuteInGameThread(function()
            inventory_classes()
            scan_scheduled = false
        end)
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
