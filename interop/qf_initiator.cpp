// A QuickFIX initiator for the interoperability test (note 006, D6): written to
// QuickFIX's documented API (quickfixengine.org: the Application callbacks,
// session settings, Session::sendToTarget), not from its source. Logs on, sends
// `orders` limit NewOrderSingles (at most 64 unanswered), counts the
// ExecutionReports by ClOrdID, then logs out. Reconnects by itself if the
// connection drops (ReconnectInterval in the settings).
//
// Usage: qf_initiator <settings file> <orders>
// Exit 0 when every order had an ExecutionReport; prints one summary line.
#include <quickfix/Application.h>
#include <quickfix/FileLog.h>
#include <quickfix/FileStore.h>
#include <quickfix/Session.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/SocketInitiator.h>
#include <quickfix/fix44/NewOrderSingle.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace {

class client : public FIX::Application {
public:
    std::atomic<bool> logged_on{false};
    std::atomic<int> logons{0}, rejects{0}, reports{0}, duplicates{0}, out_of_order{0};
    std::mutex m;
    std::set<int> answered;
    int highest = -1;
    FIX::SessionID id;

    void onCreate(const FIX::SessionID& s) override { id = s; }
    void onLogon(const FIX::SessionID&) override {
        ++logons;
        logged_on = true;
    }
    void onLogout(const FIX::SessionID&) override { logged_on = false; }
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void toApp(FIX::Message&, const FIX::SessionID&) override {}
    void fromAdmin(const FIX::Message& msg, const FIX::SessionID&) override {
        FIX::MsgType t;
        msg.getHeader().getField(t);
        if (t.getValue() == "3") ++rejects;
    }
    void fromApp(const FIX::Message& msg, const FIX::SessionID&) override {
        FIX::MsgType t;
        msg.getHeader().getField(t);
        if (t.getValue() != "8") return;
        FIX::ClOrdID c;
        msg.getField(c);
        const int n = std::atoi(c.getValue().c_str() + 1);  // "O<n>"
        std::lock_guard<std::mutex> g(m);
        if (!answered.insert(n).second) {
            ++duplicates;
            return;
        }
        if (n < highest) ++out_of_order;
        highest = n > highest ? n : highest;
        ++reports;
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: qf_initiator <settings file> <orders>\n");
        return 2;
    }
    const int orders = std::atoi(argv[2]);
    FIX::SessionSettings settings(argv[1]);
    client app;
    FIX::FileStoreFactory store(settings);
    FIX::FileLogFactory log(settings);
    FIX::SocketInitiator initiator(app, store, settings, log);
    initiator.start();
    int sent = 0;
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (app.reports < orders && std::chrono::steady_clock::now() < give_up) {
        if (app.logged_on && sent < orders && sent - app.reports < 64) {
            FIX44::NewOrderSingle o(FIX::ClOrdID("O" + std::to_string(sent)), FIX::Side(FIX::Side_BUY),
                                    FIX::TransactTime(), FIX::OrdType(FIX::OrdType_LIMIT));
            o.set(FIX::Symbol("VOD.L"));
            o.set(FIX::OrderQty(100));
            o.set(FIX::Price(72.5));
            o.set(FIX::HandlInst('1'));
            if (FIX::Session::sendToTarget(o, app.id)) ++sent;
            continue;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    initiator.stop();  // logs out
    std::printf("qf_initiator: %d orders sent, %d ExecutionReports, %d duplicates, %d out of order, %d logons, %d Rejects\n",
                sent, app.reports.load(), app.duplicates.load(), app.out_of_order.load(), app.logons.load(), app.rejects.load());
    return app.reports == orders && app.out_of_order == 0 && app.rejects == 0 ? 0 : 1;
}
