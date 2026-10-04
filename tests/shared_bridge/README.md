# Native shared-ledger bridge check

Run with the MBIIEZ dependency virtualenv Python:

```sh
/path/to/mbiiez/venv/bin/python tests/shared_bridge/check.py --mbiiez /path/to/mbiiez
```

Requires g++. Compiles the actual shared_ledger.cpp and cJSON with a small client stub,
then exercises HTTP/JSON against the actual authenticated MBIIEZ API and SQLite ledger.
Verifies login, central debit, durable earning, atomic kill/death and retry safety.
Everything uses a temporary directory and an ephemeral loopback port. It does not
start a game, read production state, install an engine or restart services.
The normal CI build separately compiles all engine hooks into the i386 engine.
