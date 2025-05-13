/**
 * @file qb/io/src/uri.cpp
 * @brief Implementation of the URI parser and manipulator
 *
 * This file contains the implementation of the uri class which provides
 * parsing and manipulation of Uniform Resource Identifiers (URIs).
 * It handles scheme, authority, path, query, and fragment components
 * according to RFC 3986.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup IO
 */

#include <qb/io/uri.h>
#include <charconv>

namespace qb::io {
DISABLE_WARNING_PUSH
DISABLE_WARNING_NARROWING
const char uri::tbl[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  -1, -1,
    -1, -1, -1, -1, -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 10, 11, 12,
    13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
DISABLE_WARNING_POP

// Standard ports for common schemes
static const qb::unordered_map<std::string_view, std::string_view> default_ports{
    // Web, HTTP and related protocols
    {"h2", "443"}, {"h3", "443"}, {"http", "80"}, {"http2", "443"}, 
    {"https", "443"}, {"quic", "443"}, {"quic-transport", "443"}, 
    {"tls", "443"}, {"ws", "80"}, {"wss", "443"},
    
    // API services and integration
    {"acme", "443"}, {"api", "80"}, {"apis", "443"}, {"graphql", "80"},
    {"graphqls", "443"}, {"grpc", "50051"}, {"grpcs", "50051"}, 
    {"rest", "80"}, {"rests", "443"}, {"thrift", "9090"}, {"thrifts", "9090"},
    {"webdav", "80"}, {"websocket", "80"}, {"websockets", "443"},
    
    // Email, messaging and collaboration
    {"imap", "143"}, {"imap2", "143"}, {"imap4", "143"}, {"imaps", "993"}, 
    {"imaps4", "993"}, {"jitsi", "443"}, {"meet", "443"}, {"pop", "110"}, 
    {"pop3", "110"}, {"pop3s", "995"}, {"pops", "995"}, {"pops3", "995"}, 
    {"rtm", "443"}, {"signal", "443"}, {"smtp", "25"}, {"smtps", "465"}, 
    {"stmp", "25"}, {"submission", "587"}, {"teams", "443"}, {"zoom", "443"},
    
    // Queue services and distributed messaging
    {"activemq", "61616"}, {"amqp", "5672"}, {"amqp-ws", "5673"}, 
    {"amqp-wss", "5673"}, {"amqps", "5671"}, {"beanstalkd", "11300"}, 
    {"celery", "6379"}, {"chronicle", "0"}, {"debezium", "8083"}, 
    {"eventstore", "2113"}, {"eventhubs", "443"}, {"gearman", "4730"}, 
    {"jgroups", "7800"}, {"jms", "61616"}, {"kafka", "9092"}, 
    {"kafka-connect", "8083"}, {"kafka-jmx", "9999"}, {"kafka-rest", "8082"}, 
    {"kafka-schema", "8081"}, {"kinesis", "443"}, {"mqtt", "1883"}, 
    {"mqtt-cluster", "1883"}, {"mqtt-tls", "8883"}, {"mqtt-ws", "8080"}, 
    {"mqtt-wss", "8081"}, {"mqtts", "8883"}, {"nanomsg", "5555"}, 
    {"nats", "4222"}, {"nats-mgmt", "8222"}, {"nats-monitoring", "8222"}, 
    {"nng", "5555"}, {"pubsub", "443"}, {"pulsar", "6650"}, {"pulsars", "6651"}, 
    {"rabbitmq", "5672"}, {"rabbitmq-management", "15672"}, {"rq", "6379"}, 
    {"sidekiq", "6379"}, {"sns", "443"}, {"solace", "55555"}, {"sqs", "443"}, 
    {"stomp", "61613"}, {"zeromq", "5555"}, {"zmq", "5555"}, {"zmq-dealer", "5559"}, 
    {"zmq-pub", "5556"}, {"zmq-rep", "5560"}, {"zmq-req", "5559"}, 
    {"zmq-router", "5560"}, {"zmq-sub", "5557"},
    
    // Databases and storage
    {"arangodb", "8529"}, {"aurora", "3306"}, {"bigtable", "443"}, 
    {"cassandra", "9042"}, {"cassandra-jmx", "7199"}, {"clickhouse", "8123"}, 
    {"cockroach", "26257"}, {"cockroachdb", "26257"}, {"cosmosdb", "443"}, 
    {"couchbase", "8091"}, {"couchbases", "18091"}, {"couchdb", "5984"}, 
    {"crdb", "26257"}, {"db", "3306"}, {"db2", "50000"}, {"dbase", "0"}, 
    {"druid", "8082"}, {"duckdb", "0"}, {"dynamodb", "443"}, {"firestore", "443"}, 
    {"graphdb", "7200"}, {"greenplum", "5432"}, {"hazelcast", "5701"}, 
    {"hbase", "16000"}, {"hbase-jmx", "10101"}, {"hive", "10000"}, 
    {"ignite", "10800"}, {"impala", "21050"}, {"influxdb", "8086"}, 
    {"janusgraph", "8182"}, {"kairosdb", "8080"}, {"kudu", "8051"}, 
    {"kylin", "7070"}, {"mariadb", "3306"}, {"memsql", "3306"}, 
    {"monetdb", "50000"}, {"mongo", "27017"}, {"mongodb", "27017"}, 
    {"mssql", "1433"}, {"mysql", "3306"}, {"neo4j", "7687"}, {"neptune", "8182"}, 
    {"nosql", "27017"}, {"opentsdb", "4242"}, {"oracle", "1521"}, 
    {"orientdb", "2424"}, {"pinot", "9000"}, {"postgres", "5432"}, 
    {"postgresql", "5432"}, {"presto", "8080"}, {"psql", "5432"}, 
    {"questdb", "8812"}, {"realm", "0"}, {"redis", "6379"}, {"rethinkdb", "28015"}, 
    {"riak", "8087"}, {"singlestore", "3306"}, {"spanner", "443"}, 
    {"sql", "3306"}, {"sqlite", "0"}, {"sqlserver", "1433"}, {"stardog", "5820"}, 
    {"surrealdb", "8000"}, {"sybase", "5000"}, {"teradata", "1025"}, 
    {"tidb", "4000"}, {"timescaledb", "5432"}, {"trino", "8080"}, 
    {"vectorwise", "27000"}, {"vertica", "5433"}, {"yugabyte", "5433"},
    
    // Cloud, serverless and infrastructure
    {"aws-iot", "8883"}, {"azure-blob", "443"}, {"azure-compute", "443"}, 
    {"azure-functions", "443"}, {"azure-iot", "8883"}, {"azure-iot-hub", "8883"}, 
    {"azure-storage", "443"}, {"calico", "179"}, {"cilium", "9962"}, 
    {"cloud-init", "80"}, {"cloud-run", "443"}, {"cloudflare", "443"}, 
    {"cloudfront", "443"}, {"cloudwatch", "443"}, {"consul", "8500"}, 
    {"consul-dns", "8600"}, {"containerd", "0"}, {"coredns", "53"}, 
    {"cri", "0"}, {"crio", "0"}, {"dcos", "80"}, {"docker", "2375"}, 
    {"docker-s", "2376"}, {"ec2", "443"}, {"ecs", "443"}, {"eks", "443"}, 
    {"etcd", "2379"}, {"etcd-client", "2379"}, {"etcd-peer", "2380"}, 
    {"gce", "443"}, {"gcf", "443"}, {"gcs", "443"}, {"gke", "443"}, 
    {"google-iot", "8883"}, {"heroku", "443"}, {"istio", "15010"}, 
    {"istio-citadel", "8060"}, {"istio-pilot", "15011"}, {"k8s", "6443"}, 
    {"kube-api", "6443"}, {"kube-controller", "10257"}, {"kube-dns", "53"}, 
    {"kube-proxy", "10256"}, {"kube-scheduler", "10259"}, {"kubelet", "10250"}, 
    {"kubernetes", "6443"}, {"lambda", "443"}, {"netlify", "443"}, 
    {"netlify-cms", "8081"}, {"nomad", "4646"}, {"nomad-http", "4646"}, 
    {"oss", "443"}, {"rancher", "80"}, {"ranchers", "443"}, {"render", "443"}, 
    {"s3", "443"}, {"s3-global", "443"}, {"s3-regional", "443"}, 
    {"swarm", "2377"}, {"vault", "8200"}, {"vercel", "443"}, {"weave", "6783"},
    
    // Monitoring, metrics and logging
    {"appdynamics", "443"}, {"collectd", "25826"}, {"cortex", "9009"}, 
    {"datadog", "443"}, {"dynatrace", "443"}, {"elastic-apm", "8200"}, 
    {"filebeat", "5066"}, {"fluentbit", "2020"}, {"fluentd", "24224"}, 
    {"ganglia", "8649"}, {"gelf", "12201"}, {"grafana", "3000"}, 
    {"grafana-live", "3000"}, {"graphite", "2003"}, {"graphite-web", "8080"}, 
    {"graylog", "9000"}, {"heartbeat", "5066"}, {"icinga2", "5665"}, 
    {"jaeger-agent", "6831"}, {"jaeger-collector", "14250"}, {"jaeger-query", "16686"}, 
    {"kapacitor", "9092"}, {"kibana", "5601"}, {"logstash", "5044"}, 
    {"loki", "3100"}, {"metricbeat", "5066"}, {"nagios", "5666"}, 
    {"newrelic", "443"}, {"opentelemetry", "4317"}, {"prometheus", "9090"}, 
    {"prometheus-alertmanager", "9093"}, {"prometheus-pushgateway", "9091"}, 
    {"rsyslog", "514"}, {"sentry", "443"}, {"splunk", "8089"}, 
    {"splunkd", "8089"}, {"stackdriver", "443"}, {"statsd", "8125"}, 
    {"syslog", "514"}, {"systemd-journal", "19532"}, {"telegraf", "8186"}, 
    {"thanos", "10901"}, {"zabbix", "10051"}, {"zabbix-agent", "10050"}, 
    {"zenoss", "8080"}, {"zipkin", "9411"},
    
    // Blockchain, crypto and decentralized systems
    {"arweave", "1984"}, {"avalanche", "9650"}, {"bitcoin", "8333"}, 
    {"bitcoin-testnet", "18333"}, {"cardano", "3001"}, {"chain", "1798"}, 
    {"chainlink", "8090"}, {"cosmos", "26656"}, {"dat", "3282"}, 
    {"eos", "8888"}, {"ethereum", "30303"}, {"ethereum-rpc", "8545"}, 
    {"filecoin", "6000"}, {"gun", "8765"}, {"holochain", "8888"}, 
    {"hyperledger", "7050"}, {"ipfs", "5001"}, {"ipfs-api", "5001"}, 
    {"ipfs-gateway", "8080"}, {"ipns", "8080"}, {"lightning", "9735"}, 
    {"monero", "18080"}, {"polkadot", "9933"}, {"ripple", "51235"}, 
    {"sia", "9980"}, {"solana", "8899"}, {"ssb", "8008"}, {"stellar", "11625"}, 
    {"storj", "7777"}, {"swarm", "1633"}, {"tron", "18888"},
    
    // Telephony and real-time communications
    {"asterisk", "5060"}, {"freepbx", "5060"}, {"h323", "1720"}, 
    {"janus", "8188"}, {"pbx", "5060"}, {"sccp", "2000"}, {"sip", "5060"}, 
    {"sips", "5061"}, {"sipml", "5060"}, {"skinny", "2000"}, {"webrtc", "443"},
    
    // Media streaming and entertainment
    {"airplay", "7000"}, {"ampache", "80"}, {"apple-music", "443"}, 
    {"chromecast", "8008"}, {"dash", "80"}, {"dashs", "443"}, 
    {"disney", "443"}, {"dlna", "8200"}, {"emby", "8096"}, 
    {"hls", "80"}, {"hlss", "443"}, {"hulu", "443"}, 
    {"icecast", "8000"}, {"jellyfin", "8096"}, {"kodi", "8080"}, 
    {"last-fm", "443"}, {"mediawiki", "80"}, {"miracast", "7236"}, 
    {"mms", "1755"}, {"mmsh", "80"}, {"mmst", "1755"}, 
    {"netflix", "443"}, {"pandora", "443"}, {"plex", "32400"}, 
    {"roku", "8060"}, {"rtmp", "1935"}, {"rtmps", "1936"}, 
    {"shoutcast", "8000"}, {"sonos", "1400"}, {"soundcloud", "443"}, 
    {"spotify", "4070"}, {"spotify-connect", "4070"}, {"tidal", "443"}, 
    {"twitch", "443"}, {"upnp-av", "8200"}, {"youtube", "443"},
    
    // Social and federated networks
    {"activitypub", "443"}, {"fediverse", "443"}, {"gitlab", "80"}, 
    {"gitlabs", "443"}, {"mastodon", "443"}, {"matrix", "8008"}, 
    {"matrixcs", "8448"}, {"matrixs", "8448"}, {"peertube", "443"}, 
    {"pixelfed", "443"},
    
    // DevOps platforms, CI/CD, deployment
    {"airflow", "8080"}, {"artifactory", "8081"}, {"chef", "10002"}, 
    {"jenkins", "8080"}, {"nexus", "8081"}, {"puppet", "8140"}, 
    {"puppet-ca", "8140"}, {"salt", "4505"}, {"sonar", "9000"}, 
    {"sonarqube", "9000"}, {"traefik", "8080"},
    
    // File systems and network storage
    {"afp", "548"}, {"c-span", "265"}, {"caldav", "80"}, {"caldavs", "443"}, 
    {"carddav", "80"}, {"carddavs", "443"}, {"ceph", "6789"}, {"cephfs", "6789"}, 
    {"cifs", "445"}, {"davs", "443"}, {"dfs", "1058"}, {"gluster", "24007"}, 
    {"gpfs", "1191"}, {"hdfs", "8020"}, {"hdfs-datanode", "50010"}, 
    {"hdfs-namenode", "8020"}, {"lustre", "988"}, {"nfs", "2049"}, 
    {"rsync", "873"}, {"smb", "445"},
    
    // Transfer and transport protocols
    {"bbftp", "5021"}, {"bittorrent", "6881"}, {"ftp", "21"}, 
    {"ftps", "990"}, {"ftps-ssl", "990"}, {"gridftp", "2811"}, 
    {"napster", "8888"}, {"sctp", "0"}, {"sftp", "22"}, {"sftp-ssh", "22"}, 
    {"ssh", "22"}, {"ssh-agent", "22"}, {"ssh-ftp", "115"}, {"tcp", "0"}, 
    {"udt", "9000"}, {"udp", "0"}, {"unix", "0"},
    
    // Security, authentication and access control
    {"cas", "8443"}, {"cryptoki", "3001"}, {"crl", "80"}, {"diameter", "3868"}, 
    {"dnscrypt", "443"}, {"dns-over-https", "443"}, {"dns-over-quic", "853"}, 
    {"dns-over-tls", "853"}, {"doh", "443"}, {"doq", "853"}, {"dot", "853"}, 
    {"dtls", "443"}, {"fido", "443"}, {"hsm", "1500"}, {"kadmin", "749"}, 
    {"kerberos", "88"}, {"kerberos-adm", "749"}, {"kerberos4", "750"}, 
    {"kpasswd", "464"}, {"krb5", "88"}, {"ldap", "389"}, {"ldaps", "636"}, 
    {"ldap-start-tls", "389"}, {"ntlm", "445"}, {"oauth", "443"}, 
    {"oauth2", "443"}, {"ocsp", "80"}, {"oidc", "443"}, {"openid", "443"}, 
    {"pkcs11", "3001"}, {"radius", "1812"}, {"radius-acct", "1813"}, 
    {"saml", "443"}, {"scep", "80"}, {"spnego", "88"}, {"tacacs", "49"}, 
    {"u2f", "443"}, {"webauthn", "443"}, {"xacml", "443"}, {"yubikey", "443"},
    
    // IoT, home automation and industrial protocols
    {"6lowpan", "51908"}, {"alexa", "443"}, {"arduino-ota", "8266"}, 
    {"bacnet", "47808"}, {"bacnet-ip", "47808"}, {"ble", "0"}, 
    {"blynk", "9443"}, {"bluetooth", "0"}, {"canopen", "1740"}, 
    {"cip", "44818"}, {"coap", "5683"}, {"coaps", "5684"}, {"coap-ws", "8080"}, 
    {"coap-wss", "4443"}, {"devicenet", "2222"}, {"dnp3", "20000"}, 
    {"doip-data", "13400"}, {"edgex", "48080"}, {"enip", "44818"}, 
    {"enocean", "11000"}, {"esphome", "6052"}, {"frigate", "5000"}, 
    {"google-home", "8008"}, {"home-bridge", "8581"}, {"homeassistant", "8123"}, 
    {"homematic", "2001"}, {"homeseer", "80"}, {"homey", "80"}, 
    {"hubitat", "80"}, {"hue", "443"}, {"iec61850", "102"}, {"ifttt", "443"}, 
    {"insteon", "25105"}, {"iobroker", "8081"}, {"knx", "3671"}, {"knxs", "3671"}, 
    {"lifx", "56700"}, {"lora", "8000"}, {"lorawan", "8000"}, {"lutron", "8083"}, 
    {"modbus", "502"}, {"modbus-tcp", "502"}, {"modbus-udp", "502"}, 
    {"node-red", "1880"}, {"onvif", "80"}, {"opc-ua", "4840"}, {"opc-ua-s", "4843"}, 
    {"openhab", "8080"}, {"platformio", "8008"}, {"profinet", "34962"}, 
    {"sigfox", "1234"}, {"siri", "443"}, {"smartthings", "443"}, 
    {"tasmota", "80"}, {"thingsboard", "8080"}, {"thingspeak", "80"}, 
    {"thread", "19789"}, {"tuya", "6668"}, {"vera", "3480"}, {"wink", "443"}, 
    {"wyze", "8888"}, {"z-wave", "44123"}, {"zigbee", "17756"}, 
    {"zigbee2mqtt", "8080"}, {"zwave", "44123"},
    
    // Web servers, frameworks and CMS
    {"apache", "80"}, {"contentful", "443"}, {"deno", "8000"}, {"django", "8000"}, 
    {"drupal", "80"}, {"flask", "5000"}, {"ghost", "2368"}, {"iis", "80"}, 
    {"jetty", "8080"}, {"joomla", "80"}, {"magento", "80"}, {"nginx", "80"}, 
    {"nodejs", "3000"}, {"prismic", "443"}, {"rails", "3000"}, {"sanity", "443"}, 
    {"shopify", "443"}, {"spring", "8080"}, {"squarespace", "443"}, 
    {"strapi", "1337"}, {"tomcat", "8080"}, {"webflow", "443"}, {"wix", "443"}, 
    {"wordpress", "80"},
    
    // Science, research and high-performance computing
    {"airflow", "8080"}, {"beowulf", "1975"}, {"boinc", "31416"}, 
    {"colab", "8081"}, {"condor", "9618"}, {"dask", "8786"}, {"flink", "8081"}, 
    {"folding", "36330"}, {"globus", "2811"}, {"grid", "2119"}, 
    {"hadoop", "8020"}, {"jupyter", "8888"}, {"kubeflow", "8080"}, 
    {"mlflow", "5000"}, {"mpi", "6551"}, {"nifi", "8080"}, {"openmpi", "6551"}, 
    {"ray", "8265"}, {"rstudio", "8787"}, {"rstudio-server", "8787"}, 
    {"sagemaker", "443"}, {"singularity", "11371"}, {"slurm", "6817"}, 
    {"spark", "7077"}, {"spark-master", "7077"}, {"spark-worker", "8081"}, 
    {"tensorflow", "6006"}, {"torch", "6006"}, {"yarn", "8050"},
    
    // DNS and service discovery
    {"bonjour", "5353"}, {"dns", "53"}, {"llmnr", "5355"}, {"mdns", "5353"}, 
    {"netbios", "137"}, {"netbios-dgm", "138"}, {"netbios-ssn", "139"}, 
    {"ssdp", "1900"}, {"upnp", "1900"}, {"zeroconf", "5353"}, {"zookeeper", "2181"}, 
    {"zookeeper-admin", "8080"},
    
    // Web server interface protocols
    {"ajp", "8009"}, {"cgi", "80"}, {"fcgi", "9000"}, {"icap", "1344"}, 
    {"scgi", "4000"}, {"uwsgi", "3031"},
    
    // Printing and scanning
    {"cpd", "9102"}, {"cups", "631"}, {"cups-ipp", "631"}, {"epsontm", "9100"}, 
    {"hplip", "1782"}, {"pjl", "9100"}, {"printer", "515"}, {"scanner", "6566"}, 
    {"twain", "9877"},
    
    // Terminals and remote access
    {"rdp", "3389"}, {"rlogin", "513"}, {"rsh", "514"}, {"telnet", "23"}, 
    {"vnc", "5900"}, {"rexec", "512"}, {"rwhod", "513"}, {"supdup", "95"}, 
    {"tn3270", "23"}, {"x11", "6000"}, {"xdmcp", "177"},
    
    // Historical or specialized services and protocols
    {"afs", "7000"}, {"appletalk", "0"}, {"archie", "1525"}, {"bgmp", "264"}, 
    {"bgp", "179"}, {"cat", "20986"}, {"chargen", "19"}, {"daytime", "13"}, 
    {"decnet", "0"}, {"discard", "9"}, {"echo", "7"}, {"finger", "79"}, 
    {"gnutella", "6346"}, {"gopher", "70"}, {"icb", "7326"}, {"imap2", "143"}, 
    {"ipmi", "623"}, {"ipx", "0"}, {"irc", "6667"}, {"ircs", "6697"}, 
    {"jabber", "5222"}, {"memoq", "2705"}, {"minecraft", "25565"}, 
    {"nntps", "563"}, {"ntp", "123"}, {"openvpn", "1194"}, {"prospero", "1525"}, 
    {"proxy", "1080"}, {"qotd", "17"}, {"quake", "26000"}, {"rpc", "111"}, 
    {"rtcp", "5005"}, {"rtp", "5004"}, {"rtsp", "554"}, {"rstp", "554"}, 
    {"snmp", "161"}, {"snmptrap", "162"}, {"socks", "1080"}, {"socks5", "1080"}, 
    {"stun", "3478"}, {"talk", "517"}, {"teamspeak", "9987"}, {"tftp", "69"}, 
    {"time", "37"}, {"trados", "53800"}, {"turn", "3478"}, {"turns", "5349"}, 
    {"uucp", "540"}, {"vmtp", "1000"}, {"wais", "210"}, {"whois", "43"}, 
    {"wireguard", "51820"}, {"x25", "1998"}, {"xliff", "80"}, {"xmpp", "5222"}, 
    {"xmpps", "5223"}, {"z39.50", "210"}
};

// Helper function to check if a character is a digit
inline bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

// Helper function to check if a character is a hexadecimal digit
inline bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Helper to extract a hex digit value
inline int hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return 0;
}

// Extract a substring between two pointers as std::string_view
inline std::string_view make_string_view(const char* begin, const char* end) {
    if (!begin || !end || begin > end)
        return {};
    return {begin, static_cast<std::size_t>(end - begin)};
}

// Constructor/Destructor implementations
uri::uri(uri &&rhs) noexcept {
    *this = std::forward<uri>(rhs);
}

uri::uri(uri const &rhs) {
    *this = rhs;
}

uri::uri(std::string const &str, int af)
    : _af(af)
    , _source(str) {
    parse();
}

uri::uri(std::string &&str, int af) noexcept
    : _af(af)
    , _source(std::move(str)) {
    parse();
}

// Enhanced parsing implementation
bool uri::parse() noexcept {
    _queries.clear();

    // If source is empty, return with default empty state
    if (_source.empty()) {
        _path = "/";
        return true;
    }

    const char *p = _source.c_str();
    const char *p_end = p + _source.length();
    
    // Storage for component pointers
    const char *scheme_begin = nullptr;
    const char *scheme_end = nullptr;
    const char *uinfo_begin = nullptr;
    const char *uinfo_end = nullptr;
    const char *host_begin = nullptr;
    const char *host_end = nullptr;
    const char *port_begin = nullptr;
    const char *port_end = nullptr;
    const char *path_begin = nullptr;
    const char *path_end = nullptr;
    const char *query_begin = nullptr;
    const char *query_end = nullptr;
    const char *fragment_begin = nullptr;
    const char *fragment_end = nullptr;

    // Determine if this is an absolute URI or a relative reference
    // by looking for a scheme (which must be followed by a colon)
    bool has_scheme = false;
    const char *p2 = p;
    
    // Scheme must start with a letter
    if (isalpha(*p2)) {
        // Check for valid scheme characters (a-z, A-Z, 0-9, +, -, .)
        const char *potential_scheme_end = p2 + 1;
        while (potential_scheme_end < p_end && 
               is_scheme_character(*potential_scheme_end)) {
            potential_scheme_end++;
        }
        
        // If we found a colon after valid scheme chars, we have a scheme
        if (potential_scheme_end < p_end && *potential_scheme_end == ':') {
            has_scheme = true;
            scheme_begin = p;
            scheme_end = potential_scheme_end;
            p = potential_scheme_end + 1; // Move past colon
        }
    }

    // Check for authority (//...)
    const char *authority_begin = nullptr;
    const char *authority_end = nullptr;
    
    if (p + 1 < p_end && p[0] == '/' && p[1] == '/') {
        // Skip "//"
        p += 2;
        authority_begin = p;
        
        // Authority continues until '/', '?', '#', or end of string
        while (p < p_end && *p != '/' && *p != '?' && *p != '#') {
            if (!is_authority_character(*p)) {
                return false; // Invalid authority character
            }
            p++;
        }
        authority_end = p;
        
        // If we have an authority, break it into components
        if (authority_begin < authority_end) {
            // Scan for user info (ends with @)
            const char *at_sign = nullptr;
            for (const char *u = authority_begin; u < authority_end; u++) {
                if (*u == '@') {
                    at_sign = u;
                    break;
                }
            }
            
            if (at_sign) {
                uinfo_begin = authority_begin;
                uinfo_end = at_sign;
                host_begin = at_sign + 1;
            } else {
                host_begin = authority_begin;
            }
            
            // Now find the port if it exists (preceded by :)
            // Start from the end of the authority and scan backwards
            const char *colon = nullptr;
            bool inside_ipv6 = false;
            
            // Handle IPv6 addresses [...]
            if (host_begin < authority_end && *host_begin == '[') {
                inside_ipv6 = true;
                const char *ipv6_end = nullptr;
                
                // Find matching ']'
                for (const char *h = host_begin + 1; h < authority_end; h++) {
                    if (*h == ']') {
                        ipv6_end = h;
                        break;
                    }
                }
                
                if (!ipv6_end) {
                    return false; // Unclosed IPv6 bracket
                }
                
                // Check for port after IPv6 (e.g., [::1]:8080)
                if (ipv6_end + 1 < authority_end && ipv6_end[1] == ':') {
                    host_end = ipv6_end + 1; // Include closing bracket
                    port_begin = ipv6_end + 2; // Skip the colon
                    port_end = authority_end;
                    
                    // Validate port (must be all digits)
                    for (const char *digit = port_begin; digit < port_end; digit++) {
                        if (!is_digit(*digit)) {
                            return false; // Invalid port
                        }
                    }
                } else {
                    host_end = ipv6_end + 1; // Include closing bracket
                }
            } else {
                // Regular host (not IPv6) - look for port
                for (const char *h = authority_end - 1; h > host_begin; h--) {
                    if (*h == ':') {
                        // Found potential port separator
                        colon = h;
                        
                        // Verify all characters after colon are digits
                        bool all_digits = true;
                        for (const char *digit = colon + 1; digit < authority_end; digit++) {
                            if (!is_digit(*digit)) {
                                all_digits = false;
                                break;
                            }
                        }
                        
                        if (all_digits && colon + 1 < authority_end) {
                            host_end = colon;
                            port_begin = colon + 1;
                            port_end = authority_end;
                        } else {
                            // Not a valid port, treat as part of hostname
                            host_end = authority_end;
                        }
                        break;
                    }
                }
                
                // No port found
                if (!colon) {
                    host_end = authority_end;
                }
            }
        }
    }
    
    // Parse path - it's everything from current position until a '?' or '#'
    if (p < p_end) {
        path_begin = p;
        
        while (p < p_end && *p != '?' && *p != '#') {
            if (!is_path_character(*p)) {
                return false; // Invalid path character
            }
            p++;
        }
        
        path_end = p;
    }
    
    // Parse query
    if (p < p_end && *p == '?') {
        p++; // Skip '?'
        query_begin = p;
        
        // For query, we're more lenient - accept all characters except '#'
        while (p < p_end && *p != '#') {
            p++;
        }
        
        query_end = p;
    }
    
    // Parse fragment
    if (p < p_end && *p == '#') {
        p++; // Skip '#'
        fragment_begin = p;
        
        // For fragment, accept all remaining characters
        fragment_end = p_end;
    }
    
    // Store the extracted components
    if (scheme_begin) {
        _scheme = make_string_view(scheme_begin, scheme_end);
        if (_scheme == "unix") {
            _af = AF_UNIX;
        }
    }
    
    if (uinfo_begin) {
        _user_info = make_string_view(uinfo_begin, uinfo_end);
    }
    
    if (host_begin) {
        _host = make_string_view(host_begin, host_end);
        
        // Determine address family if not unix
        if (_af != AF_UNIX) {
            // If we have square brackets, it's IPv6
            if (_host.size() >= 2 && _host.front() == '[' && _host.back() == ']') {
                _af = AF_INET6;
                // Strip the brackets for cleaner host access
                _host = make_string_view(host_begin + 1, host_end - 1);
            } else {
                // Check if it contains colons (IPv6) or is pure IPv4
                _af = _host.find(':') != std::string::npos ? AF_INET6 : AF_INET;
            }
        }
    }
    
    if (port_begin) {
        _port = make_string_view(port_begin, port_end);
    } else if (!_scheme.empty()) {
        // Try to use default port for known schemes
        auto it = default_ports.find(_scheme);
        if (it != std::end(default_ports)) {
            _port = it->second;
        }
    }
    
    // Always ensure path has a value - default is "/"
    if (path_begin && path_begin < path_end) {
        _path = make_string_view(path_begin, path_end);
    } else {
        _path = "/";
    }
    
    if (query_begin && query_begin < query_end) {
        // Store raw query string
        _raw_queries = make_string_view(query_begin, query_end);
        
        // Parse query parameters
        parse_query_parameters(query_begin, query_end);
    }
    
    if (fragment_begin) {
        _fragment = make_string_view(fragment_begin, fragment_end);
    }
    
    return true;
}

// Improved query parameter parsing to handle complex cases
void uri::parse_query_parameters(const char* begin, const char* end) {
    if (begin >= end) return;

    const char* param_start = begin;
    
    while (param_start < end) {
        // Skip consecutive ampersands (e.g., "?&&&key=value")
        while (param_start < end && *param_start == '&') {
            param_start++;
        }
        
        if (param_start >= end) break;
        
        // Find key-value separator (=) and parameter end (&)
        const char* equals = nullptr;
        const char* param_end = nullptr;
        
        for (const char* p = param_start; p < end; p++) {
            if (*p == '=' && !equals) {
                equals = p;
            } else if (*p == '&') {
                param_end = p;
                break;
            }
        }
        
        if (!param_end) {
            param_end = end;
        }
        
        // Extract key and value
        std::string key;
        std::string value;
        
        if (equals) {
            // Key is everything before equals sign
            if (equals > param_start) {
                key = decode(std::string_view(param_start, equals - param_start));
            } // else key is empty string ""
            
            // Value is everything after equals sign
            if (equals + 1 < param_end) {
                value = decode(std::string_view(equals + 1, param_end - (equals + 1)));
            } // else value is empty string ""
        } else {
            // No equals sign, everything is the key
            key = decode(std::string_view(param_start, param_end - param_start));
        }
        
        // Store in our map
        _queries[key].push_back(std::move(value));
        
        // Move to next parameter
        param_start = param_end + 1;
    }
}

std::string uri::decode(std::string_view input) noexcept {
    if (input.empty()) return {};
    
    std::string result;
    result.reserve(input.size());
    
    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        
        if (c == '%' && i + 2 < input.size()) {
            // Check if next two chars are valid hex digits
            if (is_hex_digit(input[i+1]) && is_hex_digit(input[i+2])) {
                unsigned char high = hex_value(input[i+1]);
                unsigned char low = hex_value(input[i+2]);
                result.push_back(static_cast<char>((high << 4) | low));
                i += 2;  // Skip the two hex digits
            } else {
                // Invalid hex digits, preserve the % character and continue
                result.push_back('%');
            }
        } else if (c == '+') {
            // In application/x-www-form-urlencoded, '+' means space
            result.push_back(' ');
        } else {
            // Normal character
            result.push_back(c);
        }
    }
    
    return result;
}

std::string uri::encode(std::string_view input) noexcept {
    if (input.empty()) return {};
    
    std::string encoded;
    encoded.reserve(input.size() * 3); // Worst case: each char becomes %XX
    
    for (size_t i = 0; i < input.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        
        if (is_unreserved(c)) {
            // Safe character, no encoding needed
            encoded.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            // Space becomes '+' in query parameters
            encoded.push_back('+');
        } else {
            // Encode as %XX
            encoded.push_back('%');
            encoded.push_back(hex[(c >> 4) & 0xF]);
            encoded.push_back(hex[c & 0xF]);
        }
    }
    
    return encoded;
}

std::string uri::decode(const char* input, std::size_t size) noexcept {
    if (!input || size == 0) return {};
    
    return decode(std::string_view(input, size));
}

std::string uri::encode(const char* input, std::size_t size) noexcept {
    if (!input || size == 0) return {};
    
    return encode(std::string_view(input, size));
}

// Factory method
uri uri::parse(std::string const &str, int af) noexcept {
    return {str, af};
}

// Assignment operators
uri& uri::operator=(std::string const &str) {
    from(str);
    return *this;
}

uri& uri::operator=(std::string &&str) noexcept {
    _source = std::move(str);
    parse();
    return *this;
}

uri& uri::operator=(uri const &rhs) {
    _source = rhs._source;
    parse();
    return *this;
}

uri& uri::operator=(uri &&rhs) noexcept {
    _source = std::move(rhs._source);
    parse();
    return *this;
}

// Utility methods
bool uri::from(std::string const &rhs) noexcept {
    _source = rhs;
    return parse();
}

bool uri::from(std::string &&rhs) noexcept {
    _source = std::move(rhs);
    return parse();
}

// Helpers for parsing/constructing URIs
bool uri::is_valid_scheme(std::string_view scheme) noexcept {
    if (scheme.empty() || !isalpha(scheme[0])) 
        return false;
    
    for (char c : scheme) {
        if (!is_scheme_character(c))
            return false;
    }
    
    return true;
}

bool uri::is_valid_host(std::string_view host) noexcept {
    if (host.empty()) 
        return false;
    
    // Check if IPv6 format
    bool is_ipv6 = false;
    if (host.size() >= 2 && host[0] == '[' && host.back() == ']') {
        is_ipv6 = true;
        // Would add IPv6 validation logic here
    }
    
    // For regular hostname or IPv4, validate each character
    for (char c : host) {
        if (!is_authority_character(c)) 
            return false;
    }
    
    return true;
}

// Improved path normalization
bool uri::normalize_path(std::string &path) noexcept {
    if (path.empty()) {
        path = "/";
        return true;
    }
    
    // Replace backslashes with forward slashes
    std::replace(path.begin(), path.end(), '\\', '/');
    
    // Optimize for simple cases
    if (path == "/" || path == ".") {
        path = "/";
        return true;
    }
    
    // Handle relative vs absolute paths
    bool is_absolute = path[0] == '/';
    
    // Split the path into segments
    std::vector<std::string> segments;
    size_t start = is_absolute ? 1 : 0;
    size_t pos;
    
    while ((pos = path.find('/', start)) != std::string::npos) {
        if (pos > start) {
            segments.push_back(path.substr(start, pos - start));
        }
        start = pos + 1;
    }
    
    // Add the last segment if it exists
    if (start < path.size()) {
        segments.push_back(path.substr(start));
    }
    
    // Process "." and ".." segments
    std::vector<std::string> normalized;
    for (const auto& segment : segments) {
        if (segment == ".") {
            // Skip current directory
        } else if (segment == "..") {
            if (!normalized.empty() && normalized.back() != "..") {
                normalized.pop_back();  // Go up one directory
            } else if (!is_absolute) {
                normalized.push_back(".."); // For relative paths, keep .. segments
            }
            // For absolute paths, ignore .. at root
        } else {
            normalized.push_back(segment);
        }
    }
    
    // Rebuild the path
    std::string result;
    
    if (is_absolute) {
        result = "/";
    }
    
    for (size_t i = 0; i < normalized.size(); ++i) {
        result += normalized[i];
        if (i < normalized.size() - 1) {
            result += "/";
        }
    }
    
    // Handle empty paths after normalization
    if (result.empty() && is_absolute) {
        result = "/";
    }
    
    path = std::move(result);
    return true;
}

} // namespace qb::io