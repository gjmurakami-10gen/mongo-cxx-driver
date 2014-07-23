TODO

    single mongod for Ruby
    spawn _exit, fork error for Ruby and Perl
    available port, also for Ruby and Perl

Questions

    why
        libEnv.AlwaysBuild(include_dbclienth_test)
        libEnv.AlwaysBuild(include_bsonh_test)
         
build

    scons --osx-version-min=10.9
    
SConscript

    unittests = [
        'dbtests/cluster_framework_test',
    ]
    
run single test example
   
    ./build/darwin/osx-version-min_10.9/mongo/dbtests/cluster_framework_test --gtest_filter=ShellTest.Basic

