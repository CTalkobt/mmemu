# Test Run Persistence with Git Hash Tracking

## Overview

Track test result history with automatic git hash recording when test state changes.

## Architecture

### Test Result History Format

```json
{
  "version": "1.0",
  "workspace": "/path/to/mmemu",
  "tests": {
    "test_memory_operations": {
      "file": "tests/test_suite.lua",
      "history": [
        {
          "timestamp": "2026-07-16T22:15:00Z",
          "gitHash": "a6e0156",
          "status": "pass",
          "duration": 42,
          "error": null
        },
        {
          "timestamp": "2026-07-16T22:00:00Z",
          "gitHash": "2d026c1",
          "status": "fail",
          "duration": 120,
          "error": "Assertion failed"
        }
      ],
      "lastStatus": "pass",
      "lastGitHash": "a6e0156",
      "statusChanges": 1
    },
    "test_register_access": {
      "file": "tests/test_suite.lua",
      "history": [
        {
          "timestamp": "2026-07-16T22:15:00Z",
          "gitHash": "a6e0156",
          "status": "pass",
          "duration": 28,
          "error": null
        }
      ],
      "lastStatus": "pass",
      "lastGitHash": "a6e0156",
      "statusChanges": 0
    }
  }
}
```

### Data Structure

```typescript
interface TestRunResult {
  timestamp: string;        // ISO 8601 timestamp
  gitHash: string;          // Abbreviated git commit hash (7 chars)
  status: 'pass' | 'fail' | 'skip';
  duration: number;         // Milliseconds
  error?: string;           // Error message if failed
}

interface TestHistory {
  file: string;
  history: TestRunResult[];
  lastStatus: 'pass' | 'fail' | 'skip';
  lastGitHash: string;
  statusChanges: number;    // Count of state transitions
}

interface TestPersistenceData {
  version: string;
  workspace: string;
  tests: Record<string, TestHistory>;
}
```

## Implementation Plan

### Phase 1: Persistence Layer (test-persistence.ts)

**Functions**:
- `getPersistencePath()` → `~/.local/share/mmemu/test-results.json`
- `loadTestResults()` → Load from file
- `saveTestResults()` → Write to file
- `recordTestRun(test, result)` → Add result and check for state change
- `getTestHistory(testName)` → Return all results for test
- `hasStatusChanged(testName, newStatus)` → Check if state changed
- `getGitHash()` → Get current commit hash
- `getStatusChangeCount(testName)` → Return transition count

**Key Features**:
- Create directory if missing
- File locking for concurrent writes
- Automatic cleanup of old data (>30 days)
- Compression of large history files

### Phase 2: Integration with Test Explorer

**Updates to test-explorer-provider.ts**:
- Load test results on startup
- Display status history in tree item
- Record results after test execution
- Show last status change info
- Display git hash in tooltip

**Tree Item Display**:
```
test_memory_operations
  ├─ Status: ✓ pass
  ├─ Duration: 42ms
  ├─ Last Change: 2026-07-16 (a6e0156)
  └─ Total Changes: 1
```

### Phase 3: UI Enhancements

**Visual Indicators**:
- `✓` Green: passing
- `✗` Red: failing
- `~` Yellow: first run or skipped
- `⚡` Blue: status changed this session

**Context Menu**:
- "View Test History" → Show all runs
- "Clear History" → Reset test data
- "Export History" → Save as CSV
- "Compare Commits" → Diff between last pass/fail

## Implementation Details

### test-persistence.ts (200 lines)

```typescript
export class TestPersistence {
  private dataPath: string;
  private data: TestPersistenceData;

  constructor(workspaceRoot?: string) {
    this.dataPath = this.getPersistencePath();
    this.data = this.loadData();
  }

  recordTestRun(test: TestCase, result: TestRunResult): boolean {
    const testKey = test.name;
    const hasChanged = this.hasStatusChanged(testKey, result.status);

    if (!this.data.tests[testKey]) {
      this.data.tests[testKey] = {
        file: test.file,
        history: [],
        lastStatus: result.status,
        lastGitHash: result.gitHash,
        statusChanges: 0,
      };
    }

    const history = this.data.tests[testKey];
    history.history.push(result);
    
    if (hasChanged) {
      history.statusChanges++;
    }

    history.lastStatus = result.status;
    history.lastGitHash = result.gitHash;

    this.save();
    return hasChanged;
  }

  hasStatusChanged(testName: string, newStatus: string): boolean {
    if (!this.data.tests[testName]) return true;
    return this.data.tests[testName].lastStatus !== newStatus;
  }

  getTestHistory(testName: string): TestRunResult[] {
    return this.data.tests[testName]?.history || [];
  }

  getStatusChangeCount(testName: string): number {
    return this.data.tests[testName]?.statusChanges || 0;
  }
}
```

### test-explorer-provider.ts Updates

**On test run completion**:
```typescript
private async recordTestResult(test: TestCase): Promise<void> {
  const gitHash = await this.getGitHash();
  const hasChanged = this.persistence.recordTestRun(test, {
    timestamp: new Date().toISOString(),
    gitHash,
    status: test.status,
    duration: test.duration!,
    error: test.error,
  });

  // If status changed, show notification
  if (hasChanged && test.status === 'pass') {
    vscode.window.showInformationMessage(
      `✓ ${test.name} fixed (${gitHash})`
    );
  } else if (hasChanged && test.status === 'fail') {
    vscode.window.showErrorMessage(
      `✗ ${test.name} regressed (${gitHash})`
    );
  }

  this.updateTreeItem(test);
}
```

## Data Storage Location

**Platform-specific paths**:
- Linux: `~/.local/share/mmemu/test-results.json`
- macOS: `~/Library/Application Support/mmemu/test-results.json`
- Windows: `%APPDATA%\mmemu\test-results.json`

**File permissions**: 0644 (readable, user-writable)
**Max file size**: 5MB (auto-cleanup beyond 2MB)

## Git Hash Integration

**Implementation**:
```typescript
async function getGitHash(): Promise<string> {
  try {
    const result = await exec('git rev-parse --short HEAD');
    return result.stdout.trim();
  } catch {
    return 'unknown';
  }
}
```

**Hash Format**: 7 characters (default short hash)
**Fallback**: 'unknown' if not in git repo

## Historical Display

### Command: View Test History

```
Test: test_memory_operations
━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Run 5: ✓ PASS    (2026-07-16 22:15)  [a6e0156]  42ms
Run 4: ✗ FAIL    (2026-07-16 22:00)  [2d026c1]  120ms (Assertion failed)
Run 3: ✓ PASS    (2026-07-16 21:45)  [8eb55b6]  38ms
Run 2: ✗ FAIL    (2026-07-16 21:30)  [d2d0506]  150ms (Array mismatch)
Run 1: ✓ PASS    (2026-07-16 21:15)  [7ac7573]  45ms

Status Changes: 3
First Status: pass
Current Status: pass
Last Changed: 2026-07-16 22:00 → 22:15
```

### Export to CSV

```csv
timestamp,git_hash,status,duration_ms,error
2026-07-16T22:15:00Z,a6e0156,pass,42,
2026-07-16T22:00:00Z,2d026c1,fail,120,Assertion failed
2026-07-16T21:45:00Z,8eb55b6,pass,38,
```

## Auto-Cleanup Policy

**Rules**:
- Remove entries older than 30 days
- Keep at least 10 most recent entries per test
- Trigger cleanup when file >2MB
- Compress old data (consolidate daily summaries)

**Cleanup Timing**:
- On startup (check file size)
- After each test run (if file >4MB)
- Manual cleanup via command

## Commands Added

```
mmemu.viewTestHistory           → Show history for selected test
mmemu.clearTestHistory          → Clear all history
mmemu.exportTestHistory         → Export to CSV
mmemu.compareTestStatus         → Diff between commits
mmemu.toggleTestHistoryDisplay  → Show/hide history info
```

## Performance Considerations

- **Read**: ~50ms for 1000 entries
- **Write**: ~10ms per test result
- **Memory**: ~1MB for 1000 tests with 100 runs each
- **Async**: All I/O operations non-blocking

## Benefits

✅ Track test reliability over time
✅ Identify which commits broke tests
✅ Detect flaky tests (frequent status changes)
✅ Build confidence in CI/CD pipeline
✅ Correlate failures with specific changes
✅ Historical record of code quality

## Integration with Existing Features

- **Test Explorer**: Display status history
- **Call Stack**: Show which commit introduced failure
- **Performance Profiler**: Track timing trends
- **Git Integration**: Link commits to test results

## Example Workflow

```
1. Run test suite
   → test_memory_operations: ✓ PASS (42ms, a6e0156)
   → test_register_access: ✓ PASS (28ms, a6e0156)

2. Make changes, run again
   → test_memory_operations: ✗ FAIL (120ms, a6e0156')
   → Shows: "Regressed since a6e0156"

3. Fix issue, run again
   → test_memory_operations: ✓ PASS (40ms, a6e0156'')
   → Shows: "Fixed! (3 changes total)"

4. View history
   → Shows all runs with git hashes
   → Can correlate with commits
```

## File Size Estimates

| Tests | Runs/Test | File Size | Cleanup |
|-------|-----------|-----------|---------|
| 10 | 100 | 50 KB | No |
| 50 | 100 | 250 KB | No |
| 100 | 100 | 500 KB | No |
| 100 | 500 | 2.5 MB | Yes (cleanup to 10 most recent) |

---

**Status**: Ready for Implementation
