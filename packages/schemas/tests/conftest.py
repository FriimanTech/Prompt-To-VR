"""Pytest config: do not auto-collect pydantic models whose name starts with 'Test'."""

collect_ignore_glob: list[str] = []

# Suppress the "cannot collect TestEntry" warning by telling pytest that
# TestEntry is not a test class.
def pytest_collection_modifyitems(items):  # pragma: no cover - harmless hook
    return None
