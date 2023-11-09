TEMPLATE = aux

cpp_build_dir = $$PWD/../../../target/cpp

makedirs.target = $$cpp_build_dir
makedirs.commands = mkdir -p $$cpp_build_dir/moc $$cpp_build_dir/obj $$cpp_build_dir/test-moc $$cpp_build_dir/test-obj $$cpp_build_dir/test-work

QMAKE_EXTRA_TARGETS += makedirs

PRE_TARGETDEPS += $$cpp_build_dir
