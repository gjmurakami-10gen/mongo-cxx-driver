TODO

    available port

build

    scons --osx-version-min=10.9
    
SConscript

    unittests = [
        'dbtests/cluster_framework_test',
    ]
    
run single test example
   
    ./build/darwin/osx-version-min_10.9/mongo/dbtests/cluster_framework_test --gtest_filter=ShellTest.Basic

