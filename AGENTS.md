使用下面的命令进行测试快照
cmake --build build --target emf2svg-conv wmf2emf-conv && ./tests/resources/snapshot.sh check [--format wmf]