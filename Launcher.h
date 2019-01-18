#pragma once

#include "Module.h"
#include <interfaces/IMemory.h>
#include <linux/cn_proc.h>

namespace WPEFramework {
namespace Plugin {

class Launcher : public PluginHost::IPlugin {
private:
    Launcher(const Launcher&) = delete;
    Launcher& operator=(const Launcher&) = delete;

    class ProcessObserver {
    private:
        ProcessObserver(const ProcessObserver&) = delete;
        ProcessObserver& operator= (const ProcessObserver&) = delete;

    public:
        class Info : public Core::ConnectorType<CN_IDX_PROC,CN_VAL_PROC> {
        private:
            Info() = delete;
            Info(const Info&) = delete;
            Info& operator= (const Info&) = delete;

        public:
            enum event {
                EVENT_NONE = proc_event::PROC_EVENT_NONE,
                EVENT_FORK = proc_event::PROC_EVENT_FORK,
                EVENT_EXEC = proc_event::PROC_EVENT_EXEC,
                EVENT_UID  = proc_event::PROC_EVENT_UID,
                EVENT_GID  = proc_event::PROC_EVENT_GID,
                EVENT_EXIT = proc_event::PROC_EVENT_EXIT
            };

        public:
            Info(const uint8_t buffer[], const uint16_t length) 
                : _status(PROC_CN_MCAST_IGNORE) {
                if (Ingest(buffer, length) == false) {
                    TRACE_L1("This failed !!!!\n");
                    _info.what = proc_event::PROC_EVENT_NONE;
                }
            }
            Info(const bool enabled) 
                : _status(enabled ? PROC_CN_MCAST_LISTEN : PROC_CN_MCAST_IGNORE) {
                _info.what = proc_event::PROC_EVENT_NONE;
            }
            virtual ~Info() {
            }

        public:
            inline event Event() const {
                return(static_cast<event>(_info.what));
            }
            inline uint32_t Id () const {
                switch (Event()) {
                case EVENT_FORK:
                    return (_info.event_data.fork.parent_pid);
                case EVENT_EXEC:
                    return (_info.event_data.exec.process_pid);
                case EVENT_UID:
                    return (_info.event_data.id.process_pid);
                case EVENT_GID:
                    return (_info.event_data.id.process_pid);
                case EVENT_EXIT:
                    return (_info.event_data.exit.process_pid);
                default:
                    break;
                }
                return(0);
            }
            inline uint32_t Group () const {
                switch (Event()) {
                case EVENT_FORK:
                    return (_info.event_data.fork.parent_tgid);
                case EVENT_EXEC:
                    return (_info.event_data.exec.process_tgid);
                case EVENT_UID:
                    return (_info.event_data.id.process_tgid);
                case EVENT_GID:
                    return (_info.event_data.id.process_tgid);
                case EVENT_EXIT:
                    return (_info.event_data.exit.process_tgid);
                default:
                    break;
                }
                return(0);
            }
            inline uint32_t ChildId () const {
                return(Event() == EVENT_FORK ? _info.event_data.fork.child_pid : 0);
            }
            inline uint32_t ChildGroup () const {
                return(Event() == EVENT_FORK ? _info.event_data.fork.child_tgid : 0);
            }
            inline uint32_t ExitCode () const {
                return(Event() == EVENT_EXIT ? _info.event_data.exit.exit_code : 0);
            }
            inline uint32_t UserId () const {
                return((Event() == EVENT_UID) || (Event() == EVENT_GID) ? _info.event_data.id.r.ruid : 0);
            }
            inline uint32_t GroupId () const {
                return((Event() == EVENT_UID) || (Event() == EVENT_GID) ? _info.event_data.id.e.egid : 0);
            }
            virtual uint16_t Message(uint8_t stream[], const uint16_t length) const override { 
    
                memcpy(stream, &_status, sizeof(_status)); 
    
                return (sizeof(_status)); 
            } 
            virtual uint16_t Message(const uint8_t stream[], const uint16_t length) override { 
                uint16_t toCopy = (length >= sizeof(proc_event) ? sizeof(proc_event) : length);
                ::memcpy(&_info, stream, toCopy);
                if (toCopy < sizeof(proc_event)) {
                    ::memset(&(reinterpret_cast<uint8_t*>(&_info)[toCopy]), 0, sizeof(proc_event) - toCopy);
                }
                return (length >= sizeof(_info.what) ? length : 0);
            }

        private:
            proc_cn_mcast_op _status;
            proc_event _info;
        };

        class Channel : public Core::SocketNetlink {
        private:
            Channel() = delete;
            Channel(const Channel&) = delete;
            Channel& operator= (const Channel&) = delete;

        public:
            Channel(ProcessObserver& parent) 
                : Core::SocketNetlink(Core::NodeId(NETLINK_CONNECTOR, 0, CN_IDX_PROC))
                , _parent(parent) {
            }
            virtual ~Channel() {
            }

        private:
            virtual uint16_t Deserialize (const uint8_t dataFrame[], const uint16_t receivedSize) {
                _parent.Received (Info(dataFrame, receivedSize));
                return (receivedSize);
            }

        private:
            ProcessObserver& _parent;
        };

    public:
        struct IProcessState {
            virtual ~IProcessState() {}

            virtual void Update(const Info&) = 0;
        };

    public:
        ProcessObserver()
            : _adminLock()
            , _channel(*this)
            , _callbacks() {
        }
        ~ProcessObserver() {
            ASSERT(_callbacks.empty());
        }

    public:
        void Register(IProcessState* observer) {
            _adminLock.Lock();
            auto found = std::find(_callbacks.begin(), _callbacks.end(), observer);
            ASSERT(found == _callbacks.end());
            if (_callbacks.empty()) {
                const bool opened = Open();
                ASSERT(opened);
            }
            _callbacks.push_back(observer);
            _adminLock.Unlock();
        }
        void Unregister(IProcessState* observer) {
            _adminLock.Lock();
            auto found = std::find(_callbacks.begin(), _callbacks.end(), observer);
            ASSERT(found != _callbacks.end());
            _callbacks.erase(found); 
            if (_callbacks.empty()) {
                Close();
            }
            _adminLock.Unlock();
        }

    private:
        bool Open() {
            bool succeeded = true;
            ASSERT (_channel.IsOpen() == false);

            if (_channel.Open(Core::infinite) == Core::ERROR_NONE) {
                Info message(true);

                if (_channel.Send(message, Core::infinite) != Core::ERROR_NONE) {
                    _channel.Close(Core::infinite);
                    succeeded = false;
                }
            }
            return (succeeded);
        }
        bool Close() {
            if (_channel.IsOpen() == true) {

                Info message(false);
                _channel.Send (message, Core::infinite);
            }
            _channel.Close(Core::infinite);

            return (Core::ERROR_NONE);
        }

    private:
        void Received (const Info& info) {

            if (!_callbacks.empty()) {
                _adminLock.Lock();

                for (auto* callback : _callbacks) {
                    callback->Update(info);
                }

                _adminLock.Unlock();
            }
        }

    private:
        Core::CriticalSection _adminLock;
        Channel _channel;
        std::vector<IProcessState*> _callbacks;
    };


    class Notification : public ProcessObserver::IProcessState {
    private:
        Notification() = delete;
        Notification(const Notification&) = delete;

    public:
        explicit Notification(Launcher* parent)
            : _parent(*parent)
        {
            ASSERT(parent != nullptr);
        }
        virtual ~Notification()
        {
            TRACE_L1("Launcher::Notification destructed. Line: %d", __LINE__);
        }

    public:
        void Update(const ProcessObserver::Info& info) override {
            _parent.Update(info);
        }

    private:
        Launcher& _parent;
    };

public:
    class Config : public Core::JSON::Container {
    private:
        Config(const Config&) = delete;
        Config& operator=(const Config&) = delete;

    public:
        class Parameter : public Core::JSON::Container {
        private:
            Parameter& operator=(const Parameter&) = delete;

        public:
            Parameter()
                : Core::JSON::Container()
                , Option()
                , Value() {
                Add(_T("option"), &Option);
                Add(_T("value"), &Value);
            }
            Parameter(const Parameter& copy) 
                : Core::JSON::Container()
                , Option(copy.Option)
                , Value(copy.Value) {
                Add(_T("option"), &Option);
                Add(_T("value"), &Value);
            }
            ~Parameter() {
            }

        public:
            Core::JSON::String Option;
            Core::JSON::String Value;
        };

    public:
        class Schedule : public Core::JSON::Container {
        private:
            Schedule& operator=(const Schedule&) = delete;

        public:
            Schedule()
                : Core::JSON::Container()
                , Time()
                , Interval() {
                Add(_T("time"), &Time);
                Add(_T("interval"), &Interval);
            }
            Schedule(const Schedule& copy)
                : Core::JSON::Container()
                , Time(copy.Time)
                , Interval(copy.Interval) {
                Add(_T("time"), &Time);
                Add(_T("interval"), &Interval);
            }
            ~Schedule() {
            }
        public:
            Core::JSON::String Time;
            Core::JSON::String Interval;
        };

    public:
        Config()
            : Core::JSON::Container()
            , Command()
            , Parameters()
            , CloseTime(3)
            , ScheduleTime()
        {
            Add(_T("command"), &Command);
            Add(_T("parameters"), &Parameters);
            Add(_T("closetime"), &CloseTime);
            Add(_T("schedule"), &ScheduleTime);
        }
        ~Config()
        {
        }

    public:
        Core::JSON::String Command;
        Core::JSON::ArrayType<Parameter> Parameters;
        Core::JSON::DecUInt8 CloseTime;
        Schedule ScheduleTime;
    };

    class Time {
public:
        Time()
        : _hour(0)
        , _minute(0)
        , _second(0)
        {
        }

        ~Time ()
        {
        }

        uint8_t Hour() { return _hour; }
        uint8_t Minute() { return _minute; }
        uint8_t Second() { return _second; }

        bool Parse(string time) {
            bool status = true;
            printf("%s:%s:%d: time = %s\n", __FILE__, __func__, __LINE__, time.c_str());

            //Get hours
            uint8_t hour;
            string hValue = Split(time, ":");
            status = IsValidTime(hValue, hour, 24);
            if (status == true) {

               //Get minutes
                uint8_t minute;
                string mValue = Split(time, ".");
                status = IsValidTime(mValue, minute, 60);
                if (status == true) {

                    //Store seconds
                    uint8_t second;
                    string sValue = time;
                    status = IsValidTime(sValue, second, 60);
                    if (status  == true) {

                        //Check all the time components are still valid
                        if ((hour > 0 && second > 0) && (minute == 0)) {
                            status = false;
                            TRACE(Trace::Information, (_T("Invalid time format")));
                        }
                        else { //Update time components
                            _hour = hour;
                            _minute = minute;
                            _second = second;
                        }
                    }
                    printf("%s:%s:%d: HH = %s MM = %s SS = %s\n", __FILE__, __func__, __LINE__, hValue.c_str(), mValue.c_str(), sValue.c_str());
                }
            }
            return status;
        }

private:
        inline bool IsDigit(const string& str) {
            return (str.find_first_not_of( "0123456789" ) == std::string::npos);
        }

        inline bool IsValidTime(const string& str, uint8_t& time, const uint8_t limit) {
            bool status = true;
            if (IsDigit(str)) {
                int t = atoi(str.c_str());
                if (t > limit || t < 0) {
                    status = false;
                    TRACE(Trace::Information, (_T("Invalid time  %s"), str.c_str()));
                }
                else {
                    time = t;
                }
            }
            else {
                status = false;
                TRACE(Trace::Information, (_T("Invalid time %s"), str.c_str()));
            }
            return status;
        }

        inline string Split(string& str, const string delimiter) {
            string word;
            size_t position = str.find(delimiter, 0);
            if (position != string::npos) {
                word = str.substr(0, position);
                str = str.substr(word.size() + 1, str.size());
            }
            printf("%s:%s:%d: %s:     %s\n", __FILE__, __func__, __LINE__, word.c_str(), str.c_str());
            return word;
        }

private:
        uint8_t _hour;
        uint8_t _minute;
        uint8_t _second;
    };

private:
    class PeriodicSync : public Core::IDispatchType<void> {
    private:
        PeriodicSync() = delete;
        PeriodicSync(const PeriodicSync&) = delete;
        PeriodicSync& operator=(const PeriodicSync&) = delete;

    public:
        PeriodicSync(Launcher* launcher)
            : _launcher(launcher)
        {
            ASSERT(launcher != nullptr);
        }
        ~PeriodicSync()
        {
        }

    public:
        virtual void Dispatch() override
        {
           Time time;
            printf("%s:%s:%d: \n", __FILE__, __func__, __LINE__);
            _launcher->LaunchJob(time);
        }

    private:
        Launcher* _launcher;
    };

public:
#ifdef __WIN32__
#pragma warning(disable : 4355)
#endif
    Launcher()
        : _service(nullptr)
        , _process(false)
        , _pid(0)
        , _closeTime(0)
        , _notification(this)
        , _memory(nullptr)
        , _time()
        , _interval()
        , _options(nullptr)
        , _client(this)
        , _activity(Core::ProxyType<PeriodicSync>::Create(_client))
    {
    }
#ifdef __WIN32__
#pragma warning(default : 4355)
#endif
    virtual ~Launcher()
    {
    }

public:
    BEGIN_INTERFACE_MAP(Launcher)
        INTERFACE_ENTRY(PluginHost::IPlugin)
        INTERFACE_AGGREGATE(Exchange::IMemory, _memory)
    END_INTERFACE_MAP

public:
    //  IPlugin methods
    // -------------------------------------------------------------------------------------------------------
    // First time initialization. Whenever a plugin is loaded, it is offered a Service object with relevant
    // information and services for this particular plugin. The Service object contains configuration information that
    // can be used to initialize the plugin correctly. If Initialization succeeds, return nothing (empty string)
    // If there is an error, return a string describing the issue why the initialisation failed.
    // The Service object is *NOT* reference counted, lifetime ends if the plugin is deactivated.
    // The lifetime of the Service object is guaranteed till the deinitialize method is called.
    const string Initialize(PluginHost::IShell* service) override;

    // The plugin is unloaded from WPEFramework. This is call allows the module to notify clients
    // or to persist information if needed. After this call the plugin will unlink from the service path
    // and be deactivated. The Service object is the same as passed in during the Initialize.
    // After theis call, the lifetime of the Service object ends.
    void Deinitialize(PluginHost::IShell* service) override;

    // Returns an interface to a JSON struct that can be used to return specific metadata information with respect
    // to this plugin. This Metadata can be used by the MetData plugin to publish this information to the ouside world.
    string Information() const override; 

    bool LaunchJob(Time time);

private:
    void Update(const ProcessObserver::Info& info);
    bool ScheduleJob(Time time);

private:
    PluginHost::IShell* _service;
    Core::Process _process;
    uint32_t _pid;
    uint8_t _closeTime;
    Core::Sink<Notification> _notification;
    Exchange::IMemory* _memory;

    Time _time;
    Time _interval;

    static ProcessObserver _observer;

    Launcher* _client;
    Core::Process::Options* _options;
    Core::ProxyType<Core::IDispatchType<void> > _activity;
};

} //namespace Plugin
} //namespace WPEFramework

