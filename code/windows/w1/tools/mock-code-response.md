Manager response:

- The user message is shown only in the right-aligned user bubble.
- The assistant card identifies the backend and timestamp.
- Long code-oriented content must stay inside the card.

```cpp
bool DispatchPrompt(const std::wstring& prompt) {
    if (prompt.empty()) return false;
    view.NewBlock(L"user");
    view.AppendText(prompt);
    backend.SendPrompt(prompt);
    return true;
}
```

Next checks:

1. Build passes.
2. Mock capture renders without overflow.
3. Real backend calls are used only for backend behavior verification.
