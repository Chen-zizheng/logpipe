#include "metrics.h"

#include <sstream>

void Metrics::reset() {
    accepted = 0;
    closed = 0;
    bytes_in = 0;
    records = 0;
    written_bytes = 0;
    dropped = 0;
    rotations = 0;
    http_requests = 0;
}

std::string Metrics::to_json() const {
    std::ostringstream os;
    os << '{'
       << "\"connections_accepted\":" << accepted.load() << ','
       << "\"connections_closed\":" << closed.load() << ','
       << "\"bytes_in\":" << bytes_in.load() << ','
       << "\"records\":" << records.load() << ','
       << "\"written_bytes\":" << written_bytes.load() << ','
       << "\"dropped\":" << dropped.load() << ','
       << "\"rotations\":" << rotations.load() << ','
       << "\"http_requests\":" << http_requests.load()
       << '}';
    return os.str();
}
