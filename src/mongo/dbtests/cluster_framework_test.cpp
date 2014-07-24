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
#include "mongo/dbtests/picojson.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <set>
#include <string>
#include <sstream>
#include <iomanip>
#include <regex>

using mongo::BSONElement;
using mongo::BSONObj;
using mongo::BSONObjIterator;
using mongo::ConnectionString;
using mongo::MockRemoteDBServer;
using mongo::MockReplicaSet;

using std::string;
using std::vector;
using std::ostream;

using namespace std;

bool warn(const string& msg)
{
    cerr << msg << ": " << strerror(errno) << endl;
    return 0;
}

bool die(const string& msg)
{
    warn(msg);
    exit(1);
    return 0;
}

vector<string> &split(const string &s, char delim, vector<string> &elems) {
    stringstream ss(s);
    string item;
    while (getline(ss, item, delim)) {
        elems.push_back(item);
    }
    return elems;
}

picojson::value toJSON(const string json)
{
    picojson::value v;
    string err;
    picojson::parse(v, json.c_str(), json.c_str() + json.length(), &err);
    err.empty() || die(err.c_str());
    return v;
}

inline stringstream& ssclear(stringstream& ss) {
   ss.str("");
   ss.clear();
   return ss;
}

string sub_to_json_start(const string s) {
    regex rx("^[^[{]*");
    return regex_replace(s, rx, string(""));
}

string gsub_isodate(const string s) {
    regex rx("ISODate\\((\".+?\")\\)");
    return regex_replace(s, rx, string("$1"));
}

string gsub_timestamp(const string s) {
    regex rx("Timestamp\\((\\d+), \\d+\\)");
    return regex_replace(s, rx, string("$1"));
}

string to_basic_json(const string s) {
    return gsub_timestamp(gsub_isodate(sub_to_json_start(s)));
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
        static const int Retries;
        static const string Prompt;
        static const string Bye;

        int sock;
        uint16_t port;

        Shell() {
            port = DefaultPort;
            connect("mongo_shell.log").read();
        }

        void spawn(const string& logFileName) {
            pid_t pid;
            (pid = fork()) != -1 || die("Shell spawn - fork error");
            if (pid == 0) {
                umask(0);
                setsid() != -1 || die("Shell spawn - error on setsid");
                int fdLog = open(logFileName.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0664);
                fdLog != -1 || die("Shell spawn - error on open of log file for mongo shell");
                dup2(fdLog, 1) != -1 || die("Shell spawn - error on dup of log file for stdout");
                dup2(fdLog, 2) != -1 || die("Shell spawn - error on dup of log file for stderr");
                close(0);
                (pid = fork()) != -1 || die("Shell spawn - fork error");
                if (pid == 0) {
                    const char *envMongoShell = getenv("MONGO_SHELL");
                    string mongoShell( envMongoShell ? envMongoShell : "../mongo/mongo");
                    execv(mongoShell.c_str(), (char *const *)spawn_argvp);
                }
                _exit(0);
            }
            int stat_loc;
            waitpid(pid, &stat_loc, 0) != -1 || warn("Shell spawn - waitpid error");
        }

        Shell& connect(const string& logFileName) {
            for (int i = 0; i < Retries; i++) {
                sock = socket(AF_INET, SOCK_STREAM, 0);
                sock != -1 || die("Shell connect - socket call error");
                struct sockaddr_in serv_addr;
                memset(&serv_addr, 0, sizeof(serv_addr));
                serv_addr.sin_family = AF_INET;
                inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) > 0 || die("inet_pton error");
                serv_addr.sin_port = htons(port);
                if (::connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == 0)
                    return *this;
                close(sock);
                spawn(logFileName);
                sleep(1);
            }
            die("Shell connect - error on connect to mongo shell");
            return *this;
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
                    warn("Shell read - count is 0 (EOF)");
                }
                else if (count == -1) {
                    warn("Shell read - socket read error");
                }
            }
        }

        Shell& puts(const string& s) {
            string t = s;
            if (*t.rbegin() != '\n')
                t += '\n';
            ssize_t ret = write(sock, t.c_str(), t.length());
            if (ret == -1)
                warn("Shell puts - write error");
            return *this;
        }

        Shell& stop(void) {
            puts("exit").read(Bye);
            shutdown(sock, SHUT_RDWR) != -1 || warn("Shell stop - shutdown error");
            close(sock) != -1 || warn("Shell stop - socket close error");
            return *this;
        }

        string x(const string& s, string prompt = Prompt) {
            return puts(s).read(prompt);
        }

        string x_s(const string& s, string prompt = Prompt) {
           string result = x(s);
           size_t pos = result.rfind(prompt);
           if (pos != string::npos)
               result.resize(pos);
           return result;
        }
        picojson::value x_json(const string& s, string prompt = Prompt) {
            return toJSON(to_basic_json(x_s(s, prompt)));
        }
        Shell& sh(const string& s, ostream& os) {
            vector<string> lines;
            split(s, '\n', lines);
            for (vector<string>::iterator is = lines.begin(); is != lines.end(); is++) {
                *is += '\n';
                os.write(is->c_str(), is->length()).flush();
                string result = x(*is);
                os.write(result.c_str(), result.length()).flush();
            }
            return *this;
        }
    };

    class ClusterTest;

    class TestNode {
    public:
        ClusterTest *cluster;
        string conn;
        string var;
        string hostPort;
        string host;
        uint16_t port;

        TestNode(ClusterTest *aCluster, const string& aConn) {
            cluster = aCluster;
            conn = aConn;
            conn.erase(remove(conn.begin(), conn.end(), '\"'), conn.end());
            //var = cluster->var;
            hostPort = conn.substr(strlen("connection to "));
            vector<string> vHostPort;
            split(hostPort, ':', vHostPort);
            host = vHostPort[0];
            port = atoi(vHostPort[1].c_str());
        }
        static vector<TestNode> vFromStringList(ClusterTest *aCluster, picojson::value pj);
    };

    vector<TestNode> TestNode::vFromStringList(ClusterTest *aCluster, picojson::value pj) {
        vector<TestNode> result;
        vector<picojson::value> pVec = pj.get<picojson::array>();
        for (vector<picojson::value>::iterator it = pVec.begin(); it != pVec.end(); it++) {
            result.push_back(TestNode(aCluster, it->get<string>()));
        }
        return result;
    }

    class ClusterTest {
    public:
        Shell *ms;
        string var;
        string opts;

        ClusterTest(Shell *aMs, string anOpts) {
            ms = aMs;
            var = "ct";
            opts = anOpts;
        }
        virtual ~ClusterTest(void) {
        }
        string x(const string& s, string prompt = Shell::Prompt) {
            return ms->x(s, prompt);
        }
        string x_s(const string& s, string prompt = Shell::Prompt) {
            return ms->x_s(s, prompt);
        }
        picojson::value x_json(const string& s, string prompt = Shell::Prompt) {
            return ms->x_json(s, prompt);
        }
        Shell& sh(const string& s, ostream& os) {
            return ms->sh(s, os);
        }
        bool exists(void) {
            string js = "typeof " + var + ";";
            return ms->x_s(js) == "object";
        }
        ClusterTest& ensureCluster(void) {
            if (exists())
                restart();
            else {
                //FileUtils.mkdir_p(@opts[:dataPath])
                start();
            }
            return *this;
        }
        virtual string start(void) {
            return string();
        }
        virtual string restart(void) {
            return string();
        }
        virtual string stop(void) {
            return string();
        }
        virtual string uri(void) {
            return string();
        }
    };

    class ReplSetTest : public ClusterTest {
    public:
        static const string Opts;

        ReplSetTest(Shell *aMs, string anOpts = Opts) : ClusterTest(aMs, anOpts) {
            var = "rs";
        }
        ~ReplSetTest(void) {

        }
        string start(void) {
            stringstream os, js;
            ssclear(js) << "var " << var << " = new ReplSetTest( " << opts << " );";
            sh(js.str(), os);
            ssclear(js) << var << ".startSet();";
            sh(js.str(), os);
            os.str().find("ReplSetTest Starting") != string::npos || die("ReplSetTest start error on startSet");
            ssclear(js) << var << ".initiate();";
            sh(js.str(), os);
            os.str().find("Config now saved locally.  Should come online in about a minute.") != string::npos || die("ReplSetTest start error on initiate");
            ssclear(js) << var << ".awaitReplication();";
            sh(js.str(), os);
            os.str().find("ReplSetTest awaitReplication: finished: all") != string::npos || die("ReplSetTest start error on awaitReplication");
            return os.str();
        }
        string stop(void) {
            stringstream os, js;
            ssclear(js) << var << ".stopSet();";
            sh(js.str(), os);
            os.str().find("ReplSetTest stopSet *** Shut down repl set - test worked ***") != string::npos || die("ReplSetTest stop error on stopSet");
            return os.str();
        }
        string restart(void) {
            stringstream os, js;
            ssclear(js) << var << ".restartSet();";
            sh(js.str(), os);
            ssclear(js) << var << ".awaitSecondaryNodes(30000);";
            sh(js.str(), os);
            ssclear(js) << var << ".awaitReplication(30000);";
            sh(js.str(), os);
            os.str().find("ReplSetTest awaitReplication: finished: all") != string::npos || die("ReplSetTest restart error on awaitReplication");
            return os.str();
        }
        picojson::value status(void) {
            stringstream js;
            ssclear(js) << var << ".status();";
            return x_json(js.str());
        }
        vector<TestNode> nodes(void) {
            stringstream js;
            ssclear(js) << var << ".nodes;";
            return TestNode::vFromStringList(this, x_json(js.str()));
        }
        TestNode primary(void) {
            stringstream js;
            ssclear(js) << var << ".getPrimary();";
            return TestNode(this, x_s(js.str()));
        }
        vector<TestNode> secondaries(void) {
            stringstream js;
            ssclear(js) << var << ".getSecondaries();";
            return TestNode::vFromStringList(this, x_json(js.str()));
        }
        string uri(void) {
            stringstream ss;
            ss << "mongodb:://";
            vector<TestNode> vNodes = nodes();
            string delim = "";
            for (vector<TestNode>::iterator it = vNodes.begin(); it != vNodes.end(); it++) {
                ss << delim << it->hostPort;
                delim = ",";
            }
            return ss.str();
        }
    };

    class ShardingTest : public ClusterTest {
    public:
        static const string Opts;

        ShardingTest(Shell *aMs, string anOpts = Opts) : ClusterTest(aMs, anOpts) {
            var = "sc";
        }
        ~ShardingTest(void) {

        }
    };

    class Orchestrator {
    public:
        enum { Single, Replica, Sharded };


        Orchestrator(int type, string opts) {

        }
        ~Orchestrator(void) {

        }
    };

    const uint16_t mongo::Shell::DefaultPort = 30001;
    const int mongo::Shell::Retries = 10;
    const string mongo::Shell::Prompt = "> ";
    const string mongo::Shell::Bye = "bye\n";

    const string mongo::ReplSetTest::Opts = "{ name: 'test', nodes: 3, startPort: 31000 }";
    const string mongo::ShardingTest::Opts = "{ name: 'test', shards: 2, rs: { nodes: 3 }, mongos: 2, other: { separateConfig: true } }";
}

namespace mongo_test {
    using namespace mongo;

    TEST(ShellTest, Basic) {
        Shell *ms = new Shell();
        string response = ms->x_s("1+2");
        ASSERT_EQUALS("3\n", response);
        ClusterTest *ct = new ClusterTest(ms, "");
        bool exists = ct->exists();
        ASSERT_EQUALS(0, exists);
        ms->stop();
        delete ct;
        delete ms;
    }
    TEST(ReplSetTest, Basic) {
        Shell *ms = new Shell();
        string sResponse;
        sResponse = ms->x_s("1+2");
        ASSERT_EQUALS("3\n", sResponse);
        ReplSetTest *rs = new ReplSetTest(ms);
        bool exists = rs->exists();
        ASSERT_EQUALS(0, exists);
        stringstream ss;
        sResponse = rs->start();

        picojson::value jsonResponse;
        jsonResponse = rs->status();
        ASSERT_EQUALS(1, (int)jsonResponse.get<picojson::object>()["myState"].get<double>());
        vector<TestNode> nodes = rs->nodes();
        ASSERT_EQUALS(3, (int)nodes.size());
        TestNode primary = rs->primary();
        ASSERT_EQUALS(31000, primary.port);
        vector<TestNode> secondaries = rs->secondaries();
        ASSERT_EQUALS(2, (int)secondaries.size());
        ASSERT_TRUE(rs->uri().find("mongodb://"));

        sResponse = rs->stop();
        ms->stop();
        delete rs;
        delete ms;
    }
    TEST(PicoJSON, Basic) {
        string json = "[\n\
                       	\"connection to osprey.local:31000\",\n\
                       	\"connection to osprey.local:31001\",\n\
                       	\"connection to osprey.local:31002\"\n\
                       ]";
        picojson::value v = toJSON(json);
        ASSERT_TRUE(v.is<picojson::array>());
        vector<picojson::value> a = v.get<picojson::array>();
        ASSERT_EQUALS(3, (int)a.size());
        ASSERT_TRUE(a.front().is<string>());
        stringstream ss;
        ss << v;
        ASSERT_EQUALS("[\"connection to osprey.local:31000\",\"connection to osprey.local:31001\",\"connection to osprey.local:31002\"]", ss.str());
    }
    TEST(ExtendedJSON, Basic) {
        string json = "xyzzy\
        {\n\
            \"set\" : \"test\",\n\
            \"date\" : ISODate(\"2014-07-24T15:02:15Z\"),\n\
            \"myState\" : 1,\n\
            \"members\" : [\n\
                {\n\
                    \"_id\" : 0,\n\
                    \"name\" : \"osprey.local:31000\",\n\
                    \"health\" : 1,\n\
                    \"state\" : 1,\n\
                    \"stateStr\" : \"PRIMARY\",\n\
                    \"uptime\" : 22,\n\
                    \"optime\" : Timestamp(1406214115, 1),\n\
                    \"optimeDate\" : ISODate(\"2014-07-24T15:01:55Z\"),\n\
                    \"electionTime\" : Timestamp(1406214125, 1),\n\
                    \"electionDate\" : ISODate(\"2014-07-24T15:02:05Z\"),\n\
                    \"self\" : true\n\
                },\n\
                {\n\
                    \"_id\" : 1,\n\
                    \"name\" : \"osprey.local:31001\",\n\
                    \"health\" : 1,\n\
                    \"state\" : 5,\n\
                    \"stateStr\" : \"STARTUP2\",\n\
                    \"uptime\" : 18,\n\
                    \"optime\" : Timestamp(0, 0),\n\
                    \"optimeDate\" : ISODate(\"1970-01-01T00:00:00Z\"),\n\
                    \"lastHeartbeat\" : ISODate(\"2014-07-24T15:02:13Z\"),\n\
                    \"lastHeartbeatRecv\" : ISODate(\"2014-07-24T15:02:14Z\"),\n\
                    \"pingMs\" : 0,\n\
                    \"lastHeartbeatMessage\" : \"initial sync need a member to be primary or secondary to do our initial sync\"\n\
                },\n\
                {\n\
                    \"_id\" : 2,\n\
                    \"name\" : \"osprey.local:31002\",\n\
                    \"health\" : 1,\n\
                    \"state\" : 5,\n\
                    \"stateStr\" : \"STARTUP2\",\n\
                    \"uptime\" : 18,\n\
                    \"optime\" : Timestamp(0, 0),\n\
                    \"optimeDate\" : ISODate(\"1970-01-01T00:00:00Z\"),\n\
                    \"lastHeartbeat\" : ISODate(\"2014-07-24T15:02:13Z\"),\n\
                    \"lastHeartbeatRecv\" : ISODate(\"2014-07-24T15:02:14Z\"),\n\
                    \"pingMs\" : 0,\n\
                    \"lastHeartbeatMessage\" : \"initial sync need a member to be primary or secondary to do our initial sync\"\n\
                }\n\
            ],\n\
            \"ok\" : 1\n\
        }\n\
        ";
        cout << "json regex:\n" << to_basic_json(json) << endl;
    }
}
