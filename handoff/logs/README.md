# Failed 0.17 notes

The user supplied build/runtime feedback during 0.17 experimentation.

Key build failures already corrected in the failed branch history:
- 0.17.0 used `QByteArrayLiteral("application/x-thispc-drag")` where Qt6's `QMimeData::setData()` MIME type parameter required a `QString`; corrected to `QStringLiteral(...)`.
- 0.17.0.3 accidentally inserted a QListWidget viewport filter into `BreadcrumbFrame`, producing `viewport was not declared in this scope` and an invalid `QListWidget::eventFilter` call; 0.17.0.4 corrected the placement.

Key runtime failure remaining in 0.17.0.4:
- drag starts;
- tab target reacts;
- main directory viewport/background still rejects drop with prohibited cursor.

Treat the runtime failure as unresolved. Start from stable `main`.
