Host-test stand-ins for the headers save_manager.cpp includes that drag in the game, SDL or the mod
loader (test-save-snapshot.sh puts this directory first on the include path). Only the names
save_manager.cpp uses are declared; save_snapshot_test.cpp defines them and injects faults.
