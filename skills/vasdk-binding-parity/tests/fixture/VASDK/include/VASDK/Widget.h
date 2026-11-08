#pragma once
class VASDK_EXPORT Widget {
public:
    struct Options {
        int speed = 1;
        void ignored();   // a nested struct's method: not Widget's
    };
    Widget();
    virtual ~Widget();
    bool start(const Options& options = {});
    void stop() { if (running()) halt(); }
    void spin();          // no row in the manifest
    void internals();
private:
    bool running() const;
    void halt();
};
