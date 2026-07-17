-- Unified Test Framework (Backend-Agnostic)
-- Issue #24 Phase 5: Backend Abstraction Layer
--
-- This framework runs tests against either emulator or hardware backend.
-- Same test code works with both backends.
--
-- Usage:
--   local TestFramework = require("test_framework")
--   local tests = TestFramework.create("emulator")
--   tests:add_test("test_name", test_function)
--   tests:run_all()

local TestFramework = {}
TestFramework.__index = TestFramework

--- Create test framework with specified backend
-- @param backend_type: "emulator" or "hardware"
-- @param backend_config: optional configuration (e.g., serial port for hardware)
function TestFramework.create(backend_type, backend_config)
    local self = setmetatable({}, TestFramework)

    local Backend = require("backend_interface")
    self.backend = Backend.create(backend_type)

    if backend_type == "hardware" then
        -- TODO: Configure hardware backend with port/baudrate
        -- self.backend = self.backend.new(backend_config.port, backend_config.baudrate)
    end

    if not self.backend:is_available() then
        error("Backend " .. backend_type .. " is not available")
    end

    self.tests = {}
    self.passed = 0
    self.failed = 0
    self.results = {}

    return self
end

--- Add test to suite
-- @param name: test name
-- @param test_func: function(backend) -> boolean (true = pass, false = fail)
function TestFramework:add_test(name, test_func)
    table.insert(self.tests, {name = name, func = test_func})
end

--- Run single test
-- @param test: {name, func} table
-- @return boolean: true if passed
function TestFramework:run_test(test)
    self.backend:log("")
    self.backend:log("[ RUN      ] " .. test.name)

    local success, result = pcall(test.func, self.backend)

    if success and result then
        self.backend:log("[       OK ] " .. test.name)
        self.passed = self.passed + 1
        self.results[test.name] = {passed = true, result = result}
        return true
    else
        self.backend:log("[    FAIL  ] " .. test.name)
        if result then
            self.backend:log("  Error: " .. tostring(result))
        end
        self.failed = self.failed + 1
        self.results[test.name] = {passed = false, error = result}
        return false
    end
end

--- Run all tests
function TestFramework:run_all()
    self.backend:log("")
    self.backend:log("================================================")
    self.backend:log("Running " .. #self.tests .. " tests on backend: " .. self.backend:name())
    self.backend:log("================================================")
    self.backend:log("")

    for _, test in ipairs(self.tests) do
        self:run_test(test)
    end

    self:print_summary()
end

--- Print test summary
function TestFramework:print_summary()
    self.backend:log("")
    self.backend:log("================================================")
    self.backend:log("Test Results (" .. self.backend:name() .. ")")
    self.backend:log("================================================")
    self.backend:log("")
    self.backend:log("Passed: " .. self.passed .. " / " .. (#self.tests))
    self.backend:log("Failed: " .. self.failed .. " / " .. (#self.tests))
    self.backend:log("")

    if self.failed == 0 then
        self.backend:log("✓ ALL TESTS PASSED")
    else
        self.backend:log("✗ SOME TESTS FAILED")
        self.backend:log("")
        self.backend:log("Failed tests:")
        for test_name, result in pairs(self.results) do
            if not result.passed then
                self.backend:log("  - " .. test_name)
                if result.error then
                    self.backend:log("    " .. tostring(result.error))
                end
            end
        end
    end

    self.backend:log("")
end

--- Get test results
-- @return {name -> {passed, result|error}}
function TestFramework:get_results()
    return self.results
end

--- Check if all tests passed
function TestFramework:all_passed()
    return self.failed == 0
end

return TestFramework
