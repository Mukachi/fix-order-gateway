// A QuickFIX acceptor for the interoperability test (note 007, S7, option A):
// written to QuickFIX's documented API (the Application callbacks, session
// settings, Session::sendToTarget), not from its source. QuickFIX's own
// examples take no cancel or replace at FIX 4.4, so this answers them: each
// limit NewOrderSingle acknowledged New, every fifth then filled at once; an
// OrderCancelRequest of a live order canceled; an OrderCancelReplaceRequest
// of a live order replaced with its new quantity; either, for an order not
// live or not known, an OrderCancelReject. QuickFIX's session layer and its
// FIX 4.4 dictionary check what arrives.
//
// FIX 4.4 values are written as the specification gives them (ExecType '5',
// and so on), so nothing depends on QuickFIX's names for them.
//
// Usage: qf_acceptor <settings file>   (runs until SIGTERM)
#include <quickfix/Application.h>
#include <quickfix/FileLog.h>
#include <quickfix/FileStore.h>
#include <quickfix/Session.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/SocketAcceptor.h>
#include <quickfix/fix44/ExecutionReport.h>
#include <quickfix/fix44/OrderCancelReject.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <map>
#include <string>
#include <thread>

namespace {

std::atomic<bool> stop{false};

class venue : public FIX::Application {
public:
    void onCreate(const FIX::SessionID&) override {}
    void onLogon(const FIX::SessionID&) override {}
    void onLogout(const FIX::SessionID&) override {}
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void toApp(FIX::Message&, const FIX::SessionID&) override {}
    void fromAdmin(const FIX::Message&, const FIX::SessionID&) override {}
    void fromApp(const FIX::Message& m, const FIX::SessionID& id) override {
        FIX::MsgType t;
        m.getHeader().getField(t);
        if (t.getValue() == "D") new_order(m, id);
        else if (t.getValue() == "F" || t.getValue() == "G") change(m, id, t.getValue() == "G");
    }

private:
    struct order {
        std::string order_id, cl_ord_id, symbol;
        char side;
        double qty, cum;
        bool live;
    };
    std::map<std::string, std::string> chain_;  // every ClOrdID of a chain to its OrderID
    std::map<std::string, order> orders_;        // by OrderID
    int ids_ = 0, execs_ = 0;

    void report(const order& o, char exec_type, char status, double last, const std::string& orig, const FIX::SessionID& id) {
        FIX44::ExecutionReport r(FIX::OrderID(o.order_id), FIX::ExecID("QE" + std::to_string(++execs_)), FIX::ExecType(exec_type),
                                 FIX::OrdStatus(status), FIX::Side(o.side), FIX::LeavesQty(o.live ? o.qty - o.cum : 0),
                                 FIX::CumQty(o.cum), FIX::AvgPx(o.cum > 0 ? 72.5 : 0));
        r.set(FIX::ClOrdID(o.cl_ord_id));
        if (!orig.empty()) r.set(FIX::OrigClOrdID(orig));
        r.set(FIX::Symbol(o.symbol));
        r.set(FIX::OrderQty(o.qty));
        if (exec_type == 'F') {
            r.set(FIX::LastQty(last));
            r.set(FIX::LastPx(72.5));
        }
        FIX::Session::sendToTarget(r, id);
    }

    void new_order(const FIX::Message& m, const FIX::SessionID& id) {
        FIX::ClOrdID c;
        FIX::Symbol sym;
        FIX::Side side;
        FIX::OrderQty qty;
        m.getField(c);
        m.getField(sym);
        m.getField(side);
        m.getField(qty);
        order o{"Q" + std::to_string(++ids_), c.getValue(), sym.getValue(), side.getValue(), qty.getValue(), 0, true};
        chain_[o.cl_ord_id] = o.order_id;
        report(o, '0', '0', 0, "", id);
        if (ids_ % 5 == 0) {  // every fifth filled at once
            const double q = o.qty;
            o.cum = q;
            o.live = false;
            report(o, 'F', '2', q, "", id);
        }
        orders_[o.order_id] = o;
    }

    void change(const FIX::Message& m, const FIX::SessionID& id, bool replace) {
        FIX::ClOrdID c;
        FIX::OrigClOrdID orig;
        m.getField(c);
        m.getField(orig);
        const auto at = chain_.find(orig.getValue());
        const char response = replace ? '2' : '1';
        if (at == chain_.end() || !orders_[at->second].live) {
            const bool known = at != chain_.end();
            FIX44::OrderCancelReject r(FIX::OrderID(known ? at->second : "NONE"), FIX::ClOrdID(c.getValue()),
                                       FIX::OrigClOrdID(orig.getValue()),
                                       FIX::OrdStatus(known ? '2' : '8'),
                                       FIX::CxlRejResponseTo(response));
            r.set(FIX::CxlRejReason(known ? 0 : 1));
            FIX::Session::sendToTarget(r, id);
            return;
        }
        order& o = orders_[at->second];
        chain_[c.getValue()] = o.order_id;
        o.cl_ord_id = c.getValue();
        if (replace) {
            FIX::OrderQty qty;
            m.getField(qty);
            o.qty = qty.getValue();
            report(o, '5', '0', 0, orig.getValue(), id);
        } else {
            o.live = false;
            report(o, '4', '4', 0, orig.getValue(), id);
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: qf_acceptor <settings file>\n");
        return 2;
    }
    std::signal(SIGTERM, [](int) { stop = true; });
    FIX::SessionSettings settings(argv[1]);
    venue app;
    FIX::FileStoreFactory store(settings);
    FIX::FileLogFactory log(settings);
    FIX::SocketAcceptor acceptor(app, store, settings, log);
    acceptor.start();
    while (!stop) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    acceptor.stop();
    return 0;
}
