// Tests for include/fixgw/acceptor_gate.hpp. Spec: docs/design/005-engine.md
// (S3; D6): each connection's first message decides it; every refusal closes
// with nothing sent; a second Logon for a connected identity is refused and
// the first is untouched.
#include "fix_text.hpp"

#include <fixgw/acceptor_gate.hpp>
#include <fixgw/transport.hpp>

#include <string>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using namespace fixgw;
using fixgw::test::check;
using refusal = acceptor_gate::refusal;

std::string logon(const std::string& sender, const std::string& target) {
    return test::message("35=A|49=" + sender + "|56=" + target + "|34=1|52=20261023-09:00:00.000|98=0|108=30|");
}

struct harness {
    tcp_listener l;
    acceptor_gate gate{512, 1'000'000'000};  // first message at most 512 bytes, 1 s of silence
    std::vector<int> admitted_fds, admitted_sessions;
    std::vector<std::string> admitted_bytes;
    std::vector<refusal> refusals;
    harness() {
        l.open(0);
        gate.add("CLIENT", "VENUE", 1);
        gate.add("CLIENT2", "VENUE", 2);
    }
    // A client connected and held by the gate; returns the client's socket.
    int connect_client() {
        const int c = connect_start(l.port());
        int a = -1;
        for (int k = 0; k < 100'000 && a < 0; ++k) a = l.accept();
        for (int k = 0; k < 100'000 && connect_check(c) == connect_status::in_progress; ++k) {
        }
        gate.hold(a, 0);
        return c;
    }
    void poll(std::int64_t now_ns) {
        for (int k = 0; k < 50; ++k)
            gate.poll(
                now_ns,
                [&](int session, int fd, std::string bytes) {
                    admitted_sessions.push_back(session);
                    admitted_fds.push_back(fd);
                    admitted_bytes.push_back(std::move(bytes));
                },
                [&](refusal r) { refusals.push_back(r); });
    }
};

void send_all(int fd, const std::string& s) {
    if (::send(fd, s.data(), s.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(s.size())) std::perror("send");
}
// True if the peer closed the connection with nothing sent before.
bool closed_silently(int fd) {
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, 1000) <= 0) return false;
    char b[64];
    return ::recv(fd, b, sizeof(b), 0) == 0;
}

void cases() {
    harness h;
    const int c1 = h.connect_client();
    const std::string m = logon("CLIENT", "VENUE");
    send_all(c1, m.substr(0, 20));
    h.poll(0);
    const bool waits = h.admitted_sessions.empty() && h.refusals.empty() && h.gate.held() == 1;
    send_all(c1, m.substr(20) + "8=FIX.4.4");  // and the start of whatever comes next
    h.poll(0);
    check(waits && h.admitted_sessions.size() == 1 && h.admitted_sessions[0] == 1 && h.admitted_bytes[0] == m + "8=FIX.4.4",
          "a Logon arriving in two pieces: held until whole, then admitted to its session with every byte read so far");

    const int c2 = h.connect_client();
    send_all(c2, logon("CLIENT", "VENUE"));
    h.poll(0);
    char b[8];
    const bool first_open = ::send(h.admitted_fds[0], "x", 1, MSG_NOSIGNAL) == 1 && ::recv(c1, b, 1, MSG_DONTWAIT) == 1;
    check(h.refusals.size() == 1 && h.refusals[0] == refusal::already_connected && closed_silently(c2) && first_open,
          "S3, D6: a second Logon for a connected identity: closed with nothing sent; the first connection untouched");

    h.gate.released(1);
    const int c3 = h.connect_client();
    send_all(c3, logon("CLIENT", "VENUE"));
    h.poll(0);
    check(h.admitted_sessions.size() == 2 && h.admitted_sessions[1] == 1, "after the session disconnects, its identity is admitted again");

    const int c4 = h.connect_client();
    send_all(c4, logon("STRANGER", "VENUE"));
    h.poll(0);
    check(h.refusals.back() == refusal::unknown_identity && closed_silently(c4), "1S c: an unknown identity: closed with nothing sent");

    const int c5 = h.connect_client();
    send_all(c5, test::message("35=D|49=CLIENT2|56=VENUE|34=1|52=20261023-09:00:00.000|11=X|"));
    h.poll(0);
    check(h.refusals.back() == refusal::not_logon && closed_silently(c5), "first message not a Logon: closed with nothing sent");

    const int c6 = h.connect_client();
    send_all(c6, "GET / HTTP/1.1\r\n\r\n");
    h.poll(0);
    check(h.refusals.back() == refusal::garbled && closed_silently(c6), "garbage: closed with nothing sent");

    const int c7 = h.connect_client();
    send_all(c7, "8=FIX.4.4\x01" "9=9000\x01" "35=A\x01" + std::string(600, 'x'));
    h.poll(0);
    check(h.refusals.back() == refusal::garbled || h.refusals.back() == refusal::too_long,
          "a first message longer than the limit: closed");
    (void)closed_silently(c7);

    const int c8 = h.connect_client();
    send_all(c8, m.substr(0, 10));
    h.poll(500'000'000);
    const bool still = h.gate.held() == 1;
    h.poll(1'000'000'000);
    check(still && h.refusals.back() == refusal::silent && closed_silently(c8), "silence past the limit: closed with nothing sent");

    const int c9 = h.connect_client();
    send_all(c9, m.substr(0, 10));
    ::shutdown(c9, SHUT_WR);
    h.poll(0);
    check(h.refusals.back() == refusal::silent && h.gate.held() == 0, "the client hangs up mid-message: closed");

    for (int fd : {c1, c2, c3, c4, c5, c6, c7, c8, c9}) ::close(fd);
    for (int fd : h.admitted_fds) ::close(fd);
}

}  // namespace

int main() {
    cases();
    return test::result();
}
