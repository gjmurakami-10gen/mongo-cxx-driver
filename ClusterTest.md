build

    scons --osx-version-min=10.9
    
SConscript

    unittests = [
        'dbtests/cluster_framework_test',
    ]
    
run
   
    ./build/darwin/osx-version-min_10.9/mongo/dbtests/cluster_framework_test

