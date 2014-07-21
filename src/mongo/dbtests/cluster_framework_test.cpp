/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/unittest/unittest.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <set>
#include <string>

using mongo::BSONElement;
using mongo::BSONObj;
using mongo::BSONObjIterator;
using mongo::ConnectionString;
using mongo::MockRemoteDBServer;
using mongo::MockReplicaSet;

using std::set;
using std::string;
using std::vector;
using std::ostream;

using namespace std;

bool die(const std::string& msg)
{
    cout << msg << ": " << strerror(errno) << endl;
    exit(1);
    return 0;
}

namespace mongo {
    const char *spawn_argvp[] = {
        "mongo",
        "--nodb",
        "--shell",
        "--listen",
        "30001",
        "src/mongo/dbtests/cluster_test.js",
        0
    };

    class Shell {
    public:
        static const uint16_t DefaultPort;
        static const string Prompt;

        int sock;
        uint16_t port;

        Shell() {
            port = DefaultPort;
        }

        void spawn(const std::string& logFileName) {
            pid_t pid;
            if ((pid = fork()) == 0) {
                int fdLog = open(logFileName.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0664);
                fdLog != -1 || die("spawn error on open of log file for mongo shell");
                dup2(fdLog, 1) != -1 || die("spawn error on dup of log file for stdout");
                dup2(fdLog, 2) != -1 || die("spawn error on dup of log file for stderr");
                int fdNull = open("/dev/null", O_RDONLY);
                fdNull != -1 || die("spawn error on open of /dev/null");
                dup2(fdNull, 0) != -1 || die("spawn error on dup of /dev/null for stdin");
                setsid() != -1 || die("spawn error on setsid");
                cout << "child dup'd" << endl;
                if ((pid = fork()) == 0) {
                    const char *envMongoShell = getenv("MONGO_SHELL");
                    string mongoShell( envMongoShell ? envMongoShell : "../mongo/mongo");
                    cout << "mongoShell: " << mongoShell << endl;
                    execv(mongoShell.c_str(), (char *const *)spawn_argvp);
                }
                exit(0);
            }
        }

        bool connect(const std::string& logFileName) {
            sock = socket(AF_INET, SOCK_STREAM, 0);
            sock != -1 || die("socket call error");
            struct sockaddr_in sock_addr;
            memset(&sock_addr, 0, sizeof(sock_addr));
            sock_addr.sin_family = AF_INET;
            sock_addr.sin_port = htons(port);
            for (int i = 0; i < 10; i++) {
                if (::connect(sock, (struct sockaddr *)&sock_addr, sizeof(sock_addr)) == 0)
                    return true;
                spawn(logFileName);
                sleep(1);
            }
            die("Error on connect to mongo shell after many retries");
            return false;
        }

        string read(string prompt = Prompt) {
            string s;
            char buf[BUFSIZ];
            ssize_t count;
            while (true) {
                count = ::read(sock, buf, BUFSIZ);
                if (count > 0) {
                    s.append(buf, count);
                    if (s.rfind(prompt) != string::npos)
                        return s;
                }
                else if (count == 0) {
                    die("count is 0 (EOF)");
                }
                else if (count == -1) {
                    die("socket read error");
                }
            }
        }
    };

    const uint16_t mongo::Shell::DefaultPort = 30001;
    const string mongo::Shell::Prompt = "> ";
}

namespace mongo_test {
    using namespace mongo;

    TEST(MockReplicaSetTest, SetName) {
        MockReplicaSet replSet("n", 3);
        ASSERT_EQUALS("n", replSet.getSetName());
        Shell *shell = new Shell();
        bool ret = shell->connect("mongo_shell.log");
        cout << "connect returned: " << ret << endl;
        string s = shell->read();
        cout << "read returned: \"" << s << "\"" << endl;
    }
}
