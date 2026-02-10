#include "media_library/media_library.hpp"
#include "media_library/config_manager.hpp"
#include "media_library/config_parser.hpp"
#include "media_library/media_library_logger.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <sstream>
#include <cstdlib>
#include <unistd.h>
#include <cstring>

#include <csignal>
#include <cstdlib> // for exit()
#include <unistd.h> // for sleep()
#include <vector>
#include <sys/types.h>
#include <sys/wait.h>
#include <gst/gst.h>
#include <glib.h>

/********************************************************************* 
 * This medialib_gst_runner tool runs Hailo GStreamer pipeline using the 
 * basic media library configuration file.
 * The app extracts the frontend config and all the encoder configs 
 * (including OSD and privacy mask), then creates up a pipeline with 
 * hailofrontendbinsrc → hailoencodebin for each stream. 
 * The encoded streams get sent over UDP and you also get FPS display for monitoring.
 * The app supports changing profile on the fly using signals.
 * 
 * "Usage: " << program_name << " <medialib_config_path> [options]" << std::endl;
 * With additional options:
 * --args_file <path> (default=None)
 * --profile <name>       Profile to use (default: current profile)
 * --udp-host <host>      UDP destination host (default:
 * --udp-port <port>      UDP destination port (default: 5000)
 *
 * In case the args file option is selected the pipeline can be 
 * triggered to change profiles at the process level. 
 * Each profile change involved a few steps: 
 * -First trigger (SIGSUSR1) stops the pipeline
 * -Update args file with new configuration and profile values.
 * -Run relevant sensor setup script: hdr/non-hdr modes (lowlight, daylight etc.):
 *   e.g run setup_imx715.sh
 * -Copy 3aconfig to /usr/bin/
 * -Update any other configuration update (e.g.  EIS on/off, dewarp on/off).
 * -Second trigger restarts the pipeline with new profile.
    *********************************************************************/



using std::vector;

volatile std::sig_atomic_t gSignalStatus = 0;
bool run_flag = true;

//static GstElement *pipeline2 = nullptr;
//static GMainLoop *loop = nullptr;



void signalHandler(int signum) {
    std::cout << "Interrupt signal (" << signum << ") received." << std::endl;
    gSignalStatus = signum;


    if (signum == SIGUSR1 || signum == SIGINT) {
        std::cout << "Restarting pipeline..." << std::endl;
        gSignalStatus = signum;
        return;
    }

    if (signum == SIGTERM  ) {
        std::cout << "Terminating the pipeline..." << std::endl;
        // Terminate program
        exit(signum); 
    }
}

// The signature for a signal handler function
typedef void (*SignalHandlerPointer)(int);

namespace fs = std::filesystem;

struct PipelineConfig
{
    std::string output_dir;
    std::string profile_name;
    std::string udp_host = "10.0.0.2";
    int udp_port = 5000;
};


void print_usage(const char *program_name)
{
    std::cerr << "Usage: " << program_name << " <medialib_config_path> [options]" << std::endl;
    std::cerr << "\nRequired:" << std::endl;
    std::cerr << "  medialib_config_path: Path to the media library config JSON file" << std::endl;
    std::cerr << "\nOptions:" << std::endl;
    std::cerr << "  --args_file <path> (default=None)" << std::endl;
    std::cerr << "  --profile <name>       Profile to use (default: current profile)" << std::endl;
    std::cerr << "  --udp-host <host>      UDP destination host (default: 127.0.0.1)" << std::endl;
    std::cerr << "  --udp-port <port>      UDP destination port (default: 5000)" << std::endl;
    std::cerr << "  -h, --help             Show this help" << std::endl;

}

vector<std::string> read_args_file_to_string(std::string file_path)
{
    std::ifstream infile(file_path);
    std::string line;
    std::vector<std::string> argslist;
    while (std::getline(infile, line))
    {
        std::istringstream iss(line);
        std::string arg;
        while (iss >> arg)
        {
            argslist.push_back(arg);
        }
    }
    return argslist;
}

bool parse_arguments(int argc, char *argv[], std::string &medialib_config_path, std::string &args_file_path, PipelineConfig &config)
{

    int argc_n = argc;
    vector<std::string> argslist = {};
    args_file_path = "";

    if (argc < 2)
    {
        print_usage(argv[0]);
        return false;
    }

    // Check for help or file first before treating argv[1] as config path
    bool file_args_used = false;
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help")
        {
            print_usage(argv[0]);
            exit(0);
        }

        if (arg == "-f" || arg == "--args_file")
        {
            args_file_path = argv[i + 1];
            std::cout << "Reading arguments from file: " << args_file_path << std::endl;
            argslist = read_args_file_to_string(args_file_path);
            argc_n = std::size(argslist);
            for (int j = 0; j < argc_n; j++)
            {
                std::cout << "Arg from file: " << argslist[j] << std::endl;
            }
           
            file_args_used = true;
            break;
        }
    }

    if (!file_args_used)
    {
        for (int j = 0; j < argc; j++) {
            std::cout << "Args from command line: " << argv[j] << std::endl;
            argslist.push_back(std::string(argv[j]));
        }
        argc_n = argc;
   

    }

    
    medialib_config_path = argslist[1];
    config.output_dir = "/tmp/medialib_gst_" + std::to_string(getpid());

    for (int i = 2; i < argc_n; i++)
    {
        std::string arg = argslist[i];
        
        if (arg == "--profile" && i + 1 < argc_n)
        {
            config.profile_name = argslist[++i];
        }
        else if (arg == "--udp-host" && i + 1 < argc_n)
        {
            config.udp_host = argslist[++i];
        }
        else if (arg == "--udp-port" && i + 1 < argc_n)
        {
            config.udp_port = std::stoi(argslist[++i]);
        }
        else
        {
            std::cerr << "Unknown argument: " << arg << std::endl;
            print_usage(argslist[0].c_str());
            return false;
        }

    }

    return true;
}

std::string read_file_to_string(const std::string &path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        throw std::runtime_error("Failed to open file: " + path);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool write_string_to_file(const std::string &content, const std::string &path)
{
    std::ofstream file(path);
    if (!file.is_open())
    {
        std::cerr << "Failed to open file for writing: " << path << std::endl;
        return false;
    }

    file << content;
    file.close();
    return true;
}

std::string build_gst_pipeline(const PipelineConfig &config, const std::string &frontend_config_path,
                               const std::vector<std::pair<std::string, std::string>> &encoder_configs,
                               const std::vector<std::string> &all_frontend_stream_ids)
{
    std::stringstream pipeline;

    std::cout << config.udp_host << std::endl;
    //pipeline << "gst-launch-1.0 -e \\\n";
    pipeline << "  hailofrontendbinsrc config-file-path=\"" << frontend_config_path << "\" name=frontend";

    std::set<std::string> encoder_stream_ids;
    for (const auto &[stream_id, encoder_path] : encoder_configs)
    {
        encoder_stream_ids.insert(stream_id);
    }

    for (size_t i = 0; i < encoder_configs.size(); i++)
    {
        const auto &[stream_id, encoder_path] = encoder_configs[i];

        pipeline << "   frontend. ! queue ! hailoencodebin config-file-path=\"" << encoder_path << "\" ! tee name=t"
                 << i;

        //pipeline << "   t" << i << ". ! queue ! h264parse name=parser config-interval=-1 ! rtph264pay ! udpsink host=" << config.udp_host
          //<< " port=" << (config.udp_port + i);
        pipeline << "   t" << i << ". ! queue ! h264parse name=parser config-interval=-1 !  video/x-h264,framerate=30/1 !";
               

        //pipeline << "  t" << i << ". ! queue ! fakesink sync=false async=false"
        //         << " name=hailo_display_" << stream_id;
    }
    
    for (const auto &stream_id : all_frontend_stream_ids)
    {
        if (encoder_stream_ids.find(stream_id) == encoder_stream_ids.end())
        {
            pipeline << "  frontend. ! queue ! fakesink sync=false async=false"
                     << " name=hailo_display_" << stream_id;
        }
    }

    return pipeline.str();
}

bool setup_profile(std::unique_ptr<ConfigManagerInteractor> &config_manager_interactor, const std::string &profile_name)
{
    if (!profile_name.empty())
    {
        auto profile_result = config_manager_interactor->switch_to_profile_by_name(profile_name);
        if (profile_result != MEDIA_LIBRARY_SUCCESS)
        {
            std::cerr << "Error: Failed to switch to profile '" << profile_name << "'" << std::endl;
            return false;
        }
        std::cout << "Using profile: " << profile_name << std::endl;
    }
    else
    {
        auto current_profile = config_manager_interactor->get_current_profile();
        if (current_profile.has_value())
        {
            std::cout << "Using current profile: " << current_profile.value()->name << std::endl;
        }
    }
    return true;
}

bool extract_configs(std::unique_ptr<ConfigManagerInteractor> &config_manager_interactor, const PipelineConfig &config,
                     std::string &frontend_config_path,
                     std::vector<std::pair<std::string, std::string>> &encoder_configs,
                     std::vector<std::string> &all_frontend_stream_ids)
{
    auto encoded_output_streams = config_manager_interactor->get_encoded_output_streams();
    std::string frontend_config_string = config_manager_interactor->get_frontend_config_as_string();

    nlohmann::json frontend_json = nlohmann::json::parse(frontend_config_string);
    nlohmann::json resolutions_array;

    if (frontend_json.contains("application_input_streams") &&
        frontend_json["application_input_streams"].contains("resolutions"))
    {
        resolutions_array = frontend_json["application_input_streams"]["resolutions"];
    }
    else if (frontend_json.contains("multi_resize_config") &&
             frontend_json["multi_resize_config"].contains("application_input_streams_config") &&
             frontend_json["multi_resize_config"]["application_input_streams_config"].contains("resolutions"))
    {
        resolutions_array = frontend_json["multi_resize_config"]["application_input_streams_config"]["resolutions"];
    }

    for (const auto &res : resolutions_array)
    {
        if (res.contains("stream_id"))
        {
            all_frontend_stream_ids.push_back(res["stream_id"].get<std::string>());
        }
    }

    frontend_config_path = config.output_dir + "/frontend_config.json";

    if (!write_string_to_file(frontend_config_string, frontend_config_path))
    {
        std::cerr << "Error: Failed to save frontend config" << std::endl;
        return false;
    }
    std::cout << "Saved frontend config to: " << frontend_config_path << std::endl;

    std::set<std::string> encoder_stream_ids;
    ConfigParser config_parser_osd = ConfigParser(ConfigSchema::CONFIG_SCHEMA_OSD);
    ConfigParser config_parser_masking = ConfigParser(ConfigSchema::CONFIG_SCHEMA_PRIVACY_MASK);

    for (const auto &entry : encoded_output_streams)
    {
        output_stream_id_t stream_id = entry.first;
        const config_encoded_output_stream_t &stream_config = entry.second;

        encoder_stream_ids.insert(stream_id);
        std::string encoder_config_string =
            std::visit([](const auto &config) -> std::string { return read_string_from_file(config.config_path); },
                       stream_config.encoding);

        nlohmann::json unified_config = nlohmann::json::parse(encoder_config_string);
        unified_config["osd"] = nlohmann::json::parse(
            config_parser_osd.config_struct_to_string<config_stream_osd_t>(stream_config.osd))["osd"];
        unified_config["privacy_mask"] = nlohmann::json::parse(
            config_parser_masking.config_struct_to_string<privacy_mask_config_t>(stream_config.masking));

        std::string unified_config_string = unified_config.dump(2);
        std::string encoder_config_path = config.output_dir + "/encoder_stream_" + stream_id + "_config.json";

        if (!write_string_to_file(unified_config_string, encoder_config_path))
        {
            std::cerr << "Error: Failed to save encoder config for stream " << stream_id << std::endl;
            return false;
        }

        std::cout << "Saved encoder config for stream " << stream_id << " to: " << encoder_config_path << std::endl;
        encoder_configs.push_back({stream_id, encoder_config_path});
    }

    return true;
}

int configHandler(SignalHandlerPointer signalHandler)
{
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa)); // Initialize struct to zero
    sa.sa_handler = signalHandler;
    sigemptyset(&(sa.sa_mask)); // Clear the signal mask

    if (sigaction(SIGUSR1, &sa, nullptr) == -1)
    {
        std::cerr << "Error: Unable to set SIGINT handler" << std::endl;
        return -1;
    }

    if (sigaction(SIGINT, &sa, nullptr) == -1)
    {
        std::cerr << "Error: Unable to set SIGINT handler" << std::endl;
        return -1;
    }

    if (sigaction(SIGTERM, &sa, nullptr) == -1)
    {
        std::cerr << "Error: Unable to set SIGTERM handler" << std::endl;
        return -1;
    }

    return 0;
}

std::string config_pipeline(PipelineConfig config, std::string medialib_config_path)
{
   try
    {
        fs::create_directories(config.output_dir);
    }
    catch (const fs::filesystem_error &e)
    {
        std::cerr << "Error: Failed to create output directory: " << e.what() << std::endl;
        return "";
    }

    try
    {
        // Read the media library config file
        std::string medialib_config_string = read_file_to_string(medialib_config_path);
        std::cout << "Loaded media library config from: " << medialib_config_path << std::endl;
        std::cout << "Config string length: " << medialib_config_string.length() << " bytes" << std::endl;

        // Create ConfigManagerInteractor
        auto config_manager_interactor_res = ConfigManagerInteractor::create(medialib_config_string);
        if (!config_manager_interactor_res.has_value())
        {
            std::cerr << "Error: Failed to create ConfigManagerInteractor" << std::endl;
            return "";
        }
        auto config_manager_interactor = std::move(config_manager_interactor_res.value());

        // Setup profile
        if (!setup_profile(config_manager_interactor, config.profile_name))
        {
            return "";
        }

        // Extract configs
        std::string frontend_config_path;
        std::vector<std::pair<std::string, std::string>> encoder_configs;
        std::vector<std::string> all_frontend_stream_ids;

        if (!extract_configs(config_manager_interactor, config, frontend_config_path, encoder_configs,
                             all_frontend_stream_ids))
        {
            return "";
        }

        // Build GStreamer pipeline
        std::string pipeline =
            build_gst_pipeline(config, frontend_config_path, encoder_configs, all_frontend_stream_ids);

        std::cout << "\n=== GStreamer Pipeline ===" << std::endl;
        std::cout << pipeline << std::endl;
        std::cout << "==========================\n" << std::endl;
        return pipeline;
    }
    catch (const std::out_of_range &e)
    {
        std::cerr << "\n[ERROR] std::out_of_range exception caught: " << e.what() << std::endl;
        std::cerr << "[ERROR] This typically means a required configuration key is missing from the JSON" << std::endl;
        std::cerr << "[ERROR] Check that all profile files exist and contain the required fields" << std::endl;
        return "";
    }
    catch (const std::exception &e)
    {
        std::cerr << "\n[ERROR] Exception caught: " << e.what() << std::endl;
        return "";
    }

}

std::vector<std::string>  get_string_vector_from_commandline(const std::string& commandline)
{
    std::istringstream iss(commandline);
    std::vector<std::string> tokens;
    std::string token;
    
    while (iss >> token)
    {
        if (token == "\\")
            continue; // Skip backslash tokens used for line continuation
        tokens.push_back(token);
    }
    
    if (tokens.empty()) return {};

    return tokens;

}

int run_pipeline(std::vector<std::string> pipe_argv)
{
    int status;
    std::cout << "Starting GStreamer pipeline..." << std::endl;
  
    std::cout << "Running main loop. Current signal status: " << gSignalStatus << std::endl;    
    pid_t child_pid = fork();

    if (child_pid < 0) {
        // Error handling
        perror("fork failed");
        return 1;
    } else if (child_pid == 0) {
        // This code runs in the child process
        std::cout << "Child process (PID: " << getpid() << ") is running a command." << std::endl;

         // 2. Convert std::vector<string> to char* array for execvp
        std::vector<char*> argv;
        for (auto& s : pipe_argv) {
            argv.push_back(&s[0]); 
        }
        argv.push_back(nullptr); // MUST be null-terminated
        execvp(argv[0], argv.data());
        perror("execlp failed");
        exit(1);
    } else {
        // This code runs in the parent process
        std::cout << "Parent process (PID: " << getpid() << ") spawned child with PID: " << child_pid << std::endl;

        while (true) {
            // Wait for child process to finish
            pid_t result = waitpid(child_pid, &status, WNOHANG);
            if (result == 0) {
                // Child is still running
                if (gSignalStatus == SIGUSR1 || gSignalStatus == SIGINT) {
                        std::cout << "Received SIGINT, restarting child process." << std::endl;
                        if (gSignalStatus == SIGUSR1) {
                            gSignalStatus = 0; // Reset signal status
                        } else if (gSignalStatus == SIGINT) {
                            run_flag = false;
                        }
                        std::cout << "Killing child process (PID: " << child_pid << ") to restart." << std::endl;
                        // Send SIGINT to the child
                        kill(child_pid, SIGINT); 

                } else {
                    sleep(1); // Sleep for a while before checking again
                }   
            } else if (result == -1) {
                // Error occurred
                perror("waitpid failed");
                break;
            } else {
                // Child has finished
                std::cout << "Child process has exited" << std::endl;
                if (WIFEXITED(status)) {
                    std::cout << "Child process exited with status: " << WEXITSTATUS(status) << std::endl;
                } else if (WIFSIGNALED(status)) {
                    std::cout << "Child process was terminated by signal: " << WTERMSIG(status) << std::endl;
                }
                break;
            }
        }
        // Optional: wait for the child process to finish
        sleep(1); // Simulate doing other work in the parent
        std::cout << "Child process finished." << std::endl;
    }
    return 0;

}



std::string main_media_runner(int argc, char *argv[])
{

    std::string medialib_config_path;
    std::string args_file_path;
    std::string pipeline;
    PipelineConfig config;

    bool first_run = true;
    bool run_flag = true;


//  int gst_cycle_main(int argc, char *argv[]) {
    //gst_init(&argc, &argv);
    //std::signal(SIGINT, handle_sigint);

    //pipeline2 = create_pipeline("10.0.0.2", 5000);
    //if (!pipeline) return -1;



    if (0 != configHandler(signalHandler))
    {
        std::cerr << "Error: Signal handler config problem" << std::endl;
        return "";
    }

    while (run_flag)
    {

        if ( ! first_run)
        {   
            //## Busy wait on second signal ##
            while (gSignalStatus != SIGUSR1 && gSignalStatus != SIGINT) {
                std::cout << "Busy waiting on another signal." << std::endl;
                sleep(1);
            }
            gSignalStatus = 0; // Reset signal status
            
        } else {
            first_run = false;
        } 

        if (!parse_arguments(argc, argv, medialib_config_path, args_file_path, config))
        {
            return "";
        }

        if (!fs::exists(medialib_config_path))
        {
            std::cerr << "Error: Media library config file does not exist: " << medialib_config_path << std::endl;
            return "";
        }
        std::cout << "Calling config pipe: MediaLib Config Path: " << medialib_config_path << std::endl;
        pipeline = config_pipeline(config, medialib_config_path);
        std::cout << "After Calling config pipe: MediaLib Config Path: " << medialib_config_path << std::endl;
        if (pipeline.empty())
        {
            return "";
        }
        else 
        return pipeline;
        /*
        vector<std::string> pipe_argv = get_string_vector_from_commandline(pipeline);
        if (pipe_argv.empty())
        {
            std::cerr << "Error: Failed to parse pipeline command line into arguments." << std::endl;
            return 1;
        }
        // Run the pipeline and monitor for signals
        std::cout << "==>>Pipe argv contents:\n" << std::endl;
        for (int i =0; i< (int) pipe_argv.size(); i++)
        {
            std::cout << "Argv[" << i << "]: " << pipe_argv[i] << std::endl;
        }

        if (args_file_path != "") {
            

            std::cout << "Running by args file change: " << args_file_path << std::endl;
            run_pipeline(pipe_argv);
        } 
        else {
            run_flag = false; // Exit after one run if no args file is used    
            std::cout << "Starting GStreamer pipeline..." << std::endl;
            run_pipeline(pipe_argv);
        }
            */
    }
    return 0;
}
