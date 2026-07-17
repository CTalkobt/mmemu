-- Performance Profiler for mmemu Lua Scripts
-- Issue #24 Phase 6.3: Performance Profiling

local profiler = {}
local stdlib = require("stdlib")

local Profiler = {}
Profiler.__index = Profiler

function Profiler.new()
    local self = setmetatable({}, Profiler)
    self.samples = {}
    self.active = false
    self.start_time = nil
    return self
end

function Profiler:start(label)
    self.active = true
    self.start_time = stdlib.time_ms()
    self.label = label or "profile"
end

function Profiler:stop()
    if not self.active then return nil end
    local elapsed = stdlib.time_ms() - self.start_time
    table.insert(self.samples, {label = self.label, time = elapsed})
    self.active = false
    return elapsed
end

function Profiler:measure(func, label)
    local start = stdlib.time_ms()
    local result = func()
    local elapsed = stdlib.time_ms() - start
    table.insert(self.samples, {label = label or "func", time = elapsed})
    return result, elapsed
end

function Profiler:stats()
    if #self.samples == 0 then return nil end
    local sum = 0
    local min = math.huge
    local max = 0
    for _, s in ipairs(self.samples) do
        sum = sum + s.time
        min = math.min(min, s.time)
        max = math.max(max, s.time)
    end
    return {
        count = #self.samples,
        total = sum,
        avg = sum / #self.samples,
        min = min,
        max = max
    }
end

function Profiler:print_report()
    local stats = self:stats()
    if not stats then return end
    print("Performance Report:")
    print("  Samples: " .. stats.count)
    print("  Total:   " .. stats.total .. " ms")
    print("  Average: " .. string.format("%.2f", stats.avg) .. " ms")
    print("  Min:     " .. stats.min .. " ms")
    print("  Max:     " .. stats.max .. " ms")
end

profiler.Profiler = Profiler
return profiler
