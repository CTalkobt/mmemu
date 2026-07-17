# Performance Tracking Separation from Test Correctness

## Overview

Decoupled performance profiling from test correctness tracking, allowing lightweight `make test` runs focused on pass/fail detection, with detailed performance profiling available via `make test-performance` for release cycle preparation.

## Architecture

### Test Correctness Tracking (Always Active)
**File**: `~/.local/share/mmemu/test-results.json`

Stores minimal data needed for regression detection:
- Test name, status (pass/fail/skip)
- Timestamp, git hash
- Duration (for timeout detection)
- Error message (if failed)
- Status change counter

**Updated by**: `make test` (every dev cycle)
**Purpose**: Identify regressions and flaky tests
**Overhead**: Minimal (~10ms per test run)

### Performance Profiling (On Demand)
**File**: `~/.local/share/mmemu/test-performance.json`

Detailed performance metrics:
- Per-function timing (call count, min/avg/max)
- Test total time
- Git hash and timestamp
- Full historical trend

**Updated by**: `make test-performance` (release cycle only)
**Purpose**: Performance regression detection, optimization opportunities
**Overhead**: Significant (requires instrumentation)

## Usage

### Regular Development
```bash
make test              # Fast, lightweight, updates test-results.json only
```

**Output**: test-results.json with correctness tracking
- See which tests regressed
- Identify status changes (pass→fail, fail→pass)
- Track test age and stability

### Release Preparation
```bash
make test-performance  # Full suite with performance profiling
```

**Output**: Both test-results.json + test-performance.json
- Full correctness validation
- Performance metrics for all tests
- Historical trend analysis
- Optimization candidates

## Data Files

### test-results.json Structure
```json
{
  "version": "1.0",
  "workspace": "/path/to/mmsim",
  "tests": {
    "cpu6502_basic": {
      "file": "src/plugins/6502/test/test_cpu6502.cpp",
      "history": [
        {
          "timestamp": "2026-07-16T10:00:00Z",
          "gitHash": "a6e0156",
          "status": "pass",
          "duration": 42,
          "error": null
        }
      ],
      "lastStatus": "pass",
      "lastGitHash": "a6e0156",
      "statusChanges": 1
    }
  }
}
```

### test-performance.json Structure
```json
{
  "version": "1.0",
  "workspace": "/path/to/mmsim",
  "tests": {
    "cpu6502_basic": [
      {
        "testName": "cpu6502_basic",
        "timestamp": "2026-07-16T10:00:00Z",
        "gitHash": "a6e0156",
        "totalMicroseconds": 42000,
        "totalMilliseconds": 42.0,
        "functions": [
          {
            "name": "MOS6502::step",
            "callCount": 1000,
            "totalMicroseconds": 30000,
            "minMicroseconds": 25,
            "maxMicroseconds": 45,
            "avgMicroseconds": 30
          }
        ]
      }
    ]
  }
}
```

## Implementation Details

### TestPersistence (Correctness)
- Lightweight, always-on
- Records status, git hash, duration, errors
- Conditional status change counter (only on transitions)
- Platform-aware file storage
- Auto-cleanup after 30 days

### TestPerformanceTracker (Performance)
- On-demand profiling via `make test-performance`
- Session-based timing (startTest/endTest handles)
- Per-function instrumentation recording
- Aggregates call counts and timing statistics
- Auto-cleanup after 90 days (longer retention for trends)

## Benefits

✅ **Fast Dev Cycle**: `make test` stays fast, no perf overhead
✅ **Clean Data**: Separate files prevent noise mixing
✅ **Release Ready**: `make test-performance` for pre-release validation
✅ **Flexible**: Enable/disable profiling without changing test code
✅ **Backward Compatible**: Existing CI/CD unchanged for normal tests
✅ **Trend Analysis**: Longer retention window for performance tracking

## Migration Path

1. **Existing CI/CD**: Continue using `make test` as-is
2. **Performance CI/CD**: Add `make test-performance` to release builds
3. **Developer Workflow**: Use `make test` normally, `make test-performance` pre-release
4. **Reporting**: test-results.json for stability, test-performance.json for perf trends

## Future Enhancements

- Performance regression thresholds (fail if slower than baseline)
- Automated performance profiling on release branches
- Dashboard integration for trend visualization
- Per-function performance budgets
- Memory usage tracking
- Cache locality analysis
