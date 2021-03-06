/*
Copyright (c) 2016,
Dan Bethell, Johannes Saam, Vahan Sosoyan, Brian Scherbinski.
All rights reserved. See COPYING.txt for more details.
*/

#include "Aton.h"
#include "FBWriter.h"
#include "FBUpdater.h"

#include "boost/format.hpp"
#include "boost/foreach.hpp"
#include "boost/regex.hpp"
#include "boost/filesystem.hpp"
#include "boost/algorithm/string.hpp"

void Aton::attach()
{
    m_legit = true;

    // Disable caching
    slowness(0);

    // Default status bar
    setStatus();

    // We don't need to see these knobs
    knob("formats_knob")->hide();
    knob("capturing_knob")->hide();
    knob("cam_fov_knob")->hide();

    for (int i=0; i<16; i++)
    {
        std::string knob_name = (boost::format("cM%s")%i).str();
        knob(knob_name.c_str())->hide();
    }
    
    // Construct full path for capturing
    m_node_name = node_name();
    using namespace boost::filesystem;
    path dir = getPath();
    path file = m_node_name + std::string(".exr");
    path fullPath = dir / file;
    std::string str_path = fullPath.string();
    boost::replace_all(str_path, "\\", "/");
    knob("path_knob")->set_text(str_path.c_str());
    
    // Check if the format is already exist
    unsigned int i;
    for (i = 0; i < Format::size(); ++i)
    {
        const char* f_name = Format::index(i)->name();
        if (f_name != NULL && m_node_name == f_name)
            m_formatExists = true;
    }
    
    if (!m_formatExists)
        m_fmt.add(m_node_name.c_str());
}

void Aton::detach()
{
    // Even though a node still exists once removed from a scene (in the
    // undo stack) we should close the port and reopen if attach() gets
    // called.
    m_legit = false;
    disconnect();
    m_node->m_frames = std::vector<double>();
    m_node->m_framebuffers = std::vector<FrameBuffer>();
}

void Aton::flagForUpdate(const Box& box)
{
    if (m_hash_count == UINT_MAX)
        m_hash_count = 0;
    else
        m_hash_count++;

    // Update the image with current bucket if given
    asapUpdate(box);
}

// We can use this to change our tcp port
void Aton::changePort(int port)
{
    m_inError = false;
    m_legit = false;
    m_connectionError = "";
    
    // Try to reconnect
    disconnect();

    try
    {
        m_server.connect(port, true);
        m_legit = true;
    }
    catch ( ... )
    {
        std::stringstream stream;
        stream << "Could not connect to port: " << port;
        m_connectionError = stream.str();
        m_inError = true;
        print_name( std::cerr );
        std::cerr << ": " << stream.str() << std::endl;
        return;
    }

    // Success
    if (m_server.isConnected())
    {
        Thread::spawn(::FBWriter, 1, this);
        Thread::spawn(::FBUpdater, 1, this);
        
        // Update port in the UI
        if (m_port != m_server.getPort())
        {
            std::stringstream stream;
            stream << (m_server.getPort());
            std::string port = stream.str();
            knob("port_number")->set_text(port.c_str());
        }
    }
}

// Disconnect the server for it's port
void Aton::disconnect()
{
    if (m_server.isConnected())
    {
        m_server.quit();
        Thread::wait(this);
    }
}

void Aton::append(Hash& hash)
{
    hash.append(m_node->m_hash_count);
    hash.append(outputContext().frame());
}

void Aton::_validate(bool for_real)
{
    // Do we need to open a port?
    if (!m_node->m_server.isConnected() && !m_inError && m_legit)
        changePort(m_port);
    
    // Handle any connection error
    if (m_inError)
        error(m_connectionError.c_str());

    if (!m_node->m_framebuffers.empty())
    {
        const int f_index = getFrameIndex(m_node->m_frames, uiContext().frame());
        FrameBuffer& fB = m_node->m_framebuffers[f_index];
        
        if (!fB.empty())
        {
            // Set the progress
            setStatus(fB.getProgress(),
                      fB.getRAM(),
                      fB.getPRAM(),
                      fB.getTime(),
                      fB.getFrame(),
                      fB.getAiVersionStr());
            
            // Set the format
            const int width = fB.getWidth();
            const int height = fB.getHeight();
            
            if (m_node->m_fmt.width() != width ||
                m_node->m_fmt.height() != height)
            {
                Format* m_fmt_ptr = &m_node->m_fmt;
                if (m_node->m_formatExists)
                {
                    bool fmtFound = false;
                    unsigned int i;
                    for (i=0; i < Format::size(); ++i)
                    {
                        const char* f_name = Format::index(i)->name();
                        if (f_name != NULL && m_node->m_node_name == f_name)
                        {
                            m_fmt_ptr = Format::index(i);
                            fmtFound = true;
                        }
                    }
                    if (!fmtFound)
                        m_fmt_ptr->add(m_node->m_node_name.c_str());
                }
                    
                m_fmt_ptr->set(0, 0, width, height);
                m_fmt_ptr->width(width);
                m_fmt_ptr->height(height);
                knob("formats_knob")->set_text(m_node->m_node_name.c_str());
            }
            
            // Set the channels
            ChannelSet& channels = m_node->m_channels;
            
            if (m_enable_aovs && fB.isReady())
            {
                const int fb_size = static_cast<int>(fB.size());
                
                if (channels.size() != fb_size)
                    channels.clear();

                for(int i = 0; i < fb_size; ++i)
                {
                    std::string bfName = fB.getBufferName(i);
                    
                    using namespace chStr;
                    if (bfName == RGBA && !channels.contains(Chan_Red))
                    {
                        channels.insert(Chan_Red);
                        channels.insert(Chan_Green);
                        channels.insert(Chan_Blue);
                        channels.insert(Chan_Alpha);
                        continue;
                    }
                    else if (bfName == Z && !channels.contains(Chan_Z))
                    {
                        channels.insert(Chan_Z);
                        continue;
                    }
                    else if (bfName == N || bfName == P)
                    {
                        if (!channels.contains(channel((bfName + _X).c_str())))
                        {
                            channels.insert(channel((bfName + _X).c_str()));
                            channels.insert(channel((bfName + _Y).c_str()));
                            channels.insert(channel((bfName + _Z).c_str()));
                        }
                        continue;
                    }
                    else if (bfName == ID)
                    {
                        if (!channels.contains(channel((bfName + _red).c_str())))
                            channels.insert(channel((bfName + _red).c_str()));
                        continue;
                    }
                    else if (!channels.contains(channel((bfName + _red).c_str())))
                    {
                        channels.insert(channel((bfName + _red).c_str()));
                        channels.insert(channel((bfName + _green).c_str()));
                        channels.insert(channel((bfName + _blue).c_str()));
                    }
                }
            }
            else
                resetChannels(channels);
        }
    }
    
    // Setup format etc
    info_.format(*m_node->m_fmtp.format());
    info_.full_size_format(*m_node->m_fmtp.fullSizeFormat());
    info_.channels(m_node->m_channels);
    info_.set(m_node->info().format());
}

void Aton::engine(int y, int x, int r, ChannelMask channels, Row& out)
{
    const int f = getFrameIndex(m_node->m_frames, uiContext().frame());
    std::vector<FrameBuffer>& fBs = m_node->m_framebuffers;
    
    foreach(z, channels)
    {
        int b = 0;
        int xx = x;
        const int c = colourIndex(z);
        float* cOut = out.writable(z) + x;
        const float* END = cOut + (r - x);
        
        ReadGuard lock(m_mutex);
        if (m_enable_aovs && !fBs.empty() && fBs[f].isReady())
            b = fBs[f].getBufferIndex(z);
        
        while (cOut < END)
        {
            if (fBs.empty() || !fBs[f].isReady() ||
                x >= fBs[f].getWidth() ||
                y >= fBs[f].getHeight() || r > fBs[f].getWidth())
            {
                *cOut = 0.0f;
            }
            else
                *cOut = fBs[f].getBufferPix(b, xx, y, c);
            ++cOut;
            ++xx;
        }
    }
}

void Aton::knobs(Knob_Callback f)
{
    // Hidden knobs
    Format_knob(f, &m_fmtp, "formats_knob", "format");
    Bool_knob(f, &m_capturing, "capturing_knob");
    Float_knob(f, &m_cam_fov, "cam_fov_knob", " cFov");
    
    // Main knobs
    Int_knob(f, &m_port, "port_number", "Port");
    Button(f, "clear_all_knob", "Clear All");

    Divider(f, "General");
    Bool_knob(f, &m_enable_aovs, "enable_aovs_knob", "Enable AOVs");
    Newline(f);
    Bool_knob(f, &m_multiframes, "multi_frame_knob", "Enable Multiple Frames");
    Newline(f);
    Knob* live_cam_knob = Bool_knob(f, &m_live_camera, "live_camera_knob", "Enable Live Camera");

    Divider(f, "Capture");
    Knob* limit_knob = Int_knob(f, &m_slimit, "limit_knob", "Limit");
    Knob* all_frames_knob = Bool_knob(f, &m_all_frames, "all_frames_knob", "Capture All Frames");
    Knob* path_knob = File_knob(f, &m_path, "path_knob", "Path");

    Newline(f);
    Knob* stamp_knob = Bool_knob(f, &m_stamp, "stamp_knob", "Frame Stamp");
    Knob* stamp_scale_knob = Float_knob(f, &m_stamp_scale, "stamp_scale_knob", "Scale");
    Knob* comment_knob = String_knob(f, &m_comment, "comment_knob", "Comment");
    Newline(f);
    Button(f, "capture_knob", "Capture");
    Button(f, "import_latest_knob", "Import latest");
    Button(f, "import_all_knob", "Import all");
    
    for (int i=0; i<16; i++)
    {
        std::string knob_name = (boost::format("cM%s")%i).str();
        Float_knob(f, &m_cam_matrix, knob_name.c_str(), knob_name.c_str());
    }

    // Status Bar knobs
    BeginToolbar(f, "status_bar");
    Knob* statusKnob = String_knob(f, &m_status, "status_knob", "");
    EndToolbar(f);

    // Set Flags
    limit_knob->set_flag(Knob::NO_RERENDER, true);
    path_knob->set_flag(Knob::NO_RERENDER, true);
    live_cam_knob->set_flag(Knob::NO_RERENDER, true);
    all_frames_knob->set_flag(Knob::NO_RERENDER, true);
    stamp_knob->set_flag(Knob::NO_RERENDER, true);
    stamp_scale_knob->set_flag(Knob::NO_RERENDER, true);
    comment_knob->set_flag(Knob::NO_RERENDER, true);
    statusKnob->set_flag(Knob::NO_RERENDER, true);
    statusKnob->set_flag(Knob::DISABLED, true);
    statusKnob->set_flag(Knob::OUTPUT_ONLY, true);
}

int Aton::knob_changed(Knob* _knob)
{
    if (_knob->is("port_number"))
    {
        changePort(m_port);
        return 1;
    }
    if (_knob->is("clear_all_knob"))
    {
        clearAllCmd();
        return 1;
    }
    if (_knob->is("multi_frame_knob"))
    {
        m_node->m_current_frame = uiContext().frame();
        return 1;
    }
    if (_knob->is("live_camera_knob"))
    {
        liveCameraToogle();
        return 1;
    }
    if (_knob->is("capture_knob"))
    {
        captureCmd();
        return 1;
    }
    if (_knob->is("stamp_knob"))
    {
        if(!m_stamp)
        {
            knob("stamp_scale_knob")->enable(false);
            knob("comment_knob")->enable(false);
        }
        else
        {
            knob("stamp_scale_knob")->enable(true);
            knob("comment_knob")->enable(true);
        }
        return 1;
    }
    if (_knob->is("import_latest_knob"))
    {
        importCmd(false);
        return 1;
    }
    if (_knob->is("import_all_knob"))
    {
        importCmd(true);
        return 1;
    }
    return 0;
}

void Aton::resetChannels(ChannelSet& channels)
{
    if (channels.size() > 4)
    {
        channels.clear();
        channels.insert(Chan_Red);
        channels.insert(Chan_Green);
        channels.insert(Chan_Blue);
        channels.insert(Chan_Alpha);
    }
}

bool Aton::isPathValid(std::string path)
{
    boost::filesystem::path filepath(path);
    boost::filesystem::path dir = filepath.parent_path();
    return boost::filesystem::exists(dir);
}

int Aton::getFrameIndex(std::vector<double>& frames, double currentFrame)
{
    int f_index = 0;
    
    if (frames.size() > 1)
    {
        if (!m_multiframes)
            currentFrame = m_node->m_current_frame;
        
        int nearFIndex = INT_MIN;
        int minFIndex = INT_MAX;
        
        ReadGuard lock(m_mutex);
        std::vector<double>::iterator it;
        for(it = frames.begin(); it != frames.end(); ++it)
        {
            if (currentFrame == *it)
            {
                f_index = static_cast<int>(it - frames.begin());
                break;
            }
            else if (currentFrame > *it && nearFIndex < *it)
            {
                nearFIndex = static_cast<int>(*it);
                f_index = static_cast<int>(it - frames.begin());
                continue;
            }
            else if (*it < minFIndex && nearFIndex == INT_MIN)
            {
                minFIndex = static_cast<int>(*it);
                f_index = static_cast<int>(it - frames.begin());
            }
        }
    }
    return f_index;
}

std::string Aton::getPath()
{
    char* aton_path = getenv("ATON_CAPTURE_PATH");
    
    // Get OS specific tmp directory path
    using namespace boost::filesystem;
    std::string def_path = temp_directory_path().string();

    if (aton_path != NULL)
        def_path = aton_path;
    
    boost::replace_all(def_path, "\\", "/");

    return def_path;
}

int Aton::getPort()
{
    const char* def_port = getenv("ATON_PORT");
    int aton_port;
    
    if (def_port == NULL)
        aton_port = 9201;
    else
        aton_port = atoi(def_port);
    
    return aton_port;
}

std::string Aton::getDateTime()
{
    // Returns date and time
    time_t rawtime;
    struct tm *timeinfo;
    char time_buffer[20];

    time (&rawtime);
    timeinfo = localtime(&rawtime);

    // Setting up the Date and Time format style
    strftime(time_buffer, 20, "%Y-%m-%d_%H-%M-%S", timeinfo);

    return std::string(time_buffer);
}

std::vector<std::string> Aton::getCaptures()
{
    // Our captured filenames list
    std::vector<std::string> results;
    
    // If the directory exist
    if (isPathValid(m_path))
    {
        using namespace boost::filesystem;
        path filepath(m_path);
        directory_iterator it(filepath.parent_path());
        directory_iterator end;

        // Regex expression to find captured files
        std::string exp = ( boost::format("%s.+.%s")%filepath.stem().string()
                                                    %filepath.extension().string() ).str();
        const boost::regex filter(exp);

        // Iterating through directory to find matching files
        BOOST_FOREACH(path const& p, std::make_pair(it, end))
        {
            if(is_regular_file(p))
            {
                boost::match_results<std::string::const_iterator> what;
                if (boost::regex_search(it->path().filename().string(),
                                        what, filter, boost::match_default))
                {
                    std::string res = p.filename().string();
                    results.push_back(res);
                }
            }
        }
    }
    return results;
}

void Aton::cleanByLimit()
{
    if (!m_garbageList.empty())
    {
        // In windows sometimes files can't be deleted due to lack of
        // access so we collecting a garbage list and trying to remove
        // them next time when user make a capture
        std::vector<std::string>::iterator it;
        for(it = m_garbageList.begin(); it != m_garbageList.end(); ++it)
            std::remove(it->c_str());
    }

    std::vector<std::string> captures = getCaptures();
    
    using namespace boost::filesystem;
    path filepath(m_path);
    path dir = filepath.parent_path();

    // Reverse iterating through file list
    if (!captures.empty())
    {
        std::vector<std::string>::reverse_iterator it;
        for(it = captures.rbegin(); it != captures.rend(); ++it)
        {
            path file = *it;
            path path = dir / file;
            std::string str_path = path.string();
            boost::replace_all(str_path, "\\", "/");

            // Remove the file if it's out of limit
            if ((it - captures.rbegin()) >= m_slimit)
            {
                if (std::remove(str_path.c_str()) != 0)
                    m_garbageList.push_back(str_path);

                std::string cmd; // Our python command buffer
                // Remove appropriate Read nodes as well
                cmd = ( boost::format("exec('''for i in nuke.allNodes('Read'):\n\t"
                                                  "if '%s' == i['file'].value():\n\t\t"
                                                      "nuke.delete(i)''')")%str_path ).str();
                script_command(cmd.c_str(), true, false);
                script_unlock();
            }
        }
    }
}

void Aton::clearAllCmd()
{
    std::vector<FrameBuffer>& fBs  = m_node->m_framebuffers;
    std::vector<double>& frames  = m_node->m_frames;

    if (!fBs.empty() && !frames.empty())
    {
        std::vector<FrameBuffer>::iterator it;
        for(it = fBs.begin(); it != fBs.end(); ++it)
            it->ready(false);
        
        m_node->m_legit = false;
        m_node->disconnect();
        
        fBs =  std::vector<FrameBuffer>();
        frames = std::vector<double>();
        
        resetChannels(m_node->m_channels);
        m_node->m_legit = true;
        
        flagForUpdate();
        setStatus();
    }
}

void Aton::captureCmd()
{
    std::string path = std::string(m_path);

    if (m_node->m_frames.size() > 0 && isPathValid(path) && m_slimit > 0)
    {
        // Add date or frame suffix to the path
        std::string key (".");
        std::string timeFrameSuffix;
        std::string frames;
        double startFrame;
        double endFrame;
        
        std::vector<double> sortedFrames = m_node->m_frames;
        std::stable_sort(sortedFrames.begin(), sortedFrames.end());

        if (m_multiframes && m_all_frames)
        {
            timeFrameSuffix += "_" + std::string("####");
            startFrame = sortedFrames.front();
            endFrame = sortedFrames.back();
            
            std::vector<double>::iterator it;
            for(it = sortedFrames.begin(); it != sortedFrames.end(); ++it)
                frames += (boost::format("%s,")%*it).str();
            
            frames.resize(frames.size() - 1);
        }
        else
        {
            timeFrameSuffix += "_" + getDateTime();
            startFrame = endFrame = uiContext().frame();
            frames = (boost::format("%s")%uiContext().frame()).str();
        }

        timeFrameSuffix += ".";
        std::size_t found = path.rfind(key);
        if (found != std::string::npos)
            path.replace(found, key.length(), timeFrameSuffix);

        std::string cmd; // Our python command buffer
        // Create a Write node and return it's name
        cmd = (boost::format("nuke.nodes.Write(file='%s').name()")%path.c_str()).str();
        script_command(cmd.c_str());
        std::string writeNodeName = script_result();
        script_unlock();

        // Connect to Write node
        cmd = (boost::format("nuke.toNode('%s').setInput(0, nuke.toNode('%s'));"
                             "nuke.toNode('%s')['channels'].setValue('all');"
                             "nuke.toNode('%s')['afterRender']."
                             "setValue('''nuke.nodes.Read(file='%s', first=%s, last=%s, on_error=3)''')")%writeNodeName
                                                                                                         %m_node->m_node_name
                                                                                                         %writeNodeName
                                                                                                         %writeNodeName
                                                                                                         %path.c_str()
                                                                                                         %startFrame
                                                                                                         %endFrame).str();
        script_command(cmd.c_str(), true, false);
        script_unlock();
        
        if (m_stamp)
        {
            double fontSize = m_stamp_scale * 0.12;
        
            // Add text node in between to put a stamp on the capture
            cmd = (boost::format("stamp = nuke.nodes.Text2();"
                                 "stamp['message'].setValue('''[python {nuke.toNode('%s')['status_knob'].value()}] | Comment: %s''');"
                                 "stamp['global_font_scale'].setValue(%s);"
                                 "stamp['yjustify'].setValue('bottom');"
                                 "stamp['color'].setValue(0.5);"
                                 "stamp['enable_background'].setValue(True);"
                                 "stamp['background_color'].setValue([0.05, 0.05, 0.05, 1]);"
                                 "stamp['background_opacity'].setValue(0.9);"
                                 "stamp['background_border_x'].setValue(10000);"
                                 "stamp.setInput(0, nuke.toNode('%s'));"
                                 "nuke.toNode('%s').setInput(0, stamp)")%m_node->m_node_name
                                                                        %m_comment
                                                                        %fontSize
                                                                        %m_node->m_node_name
                                                                        %writeNodeName ).str();
            script_command(cmd.c_str(), true, false);
            script_unlock();
        }

        // Execute the Write node
        cmd = (boost::format("exec('''import thread\n"
                                     "def writer():\n\t"
                                         "def status(b):\n\t\t"
                                             "nuke.toNode('%s')['capturing_knob'].setValue(b)\n\t\t"
                                             "if not b:\n\t\t\t"
                                                 "if %s:\n\t\t\t\t"
                                                    "nuke.delete(nuke.toNode('%s').input(0))\n\t\t\t"
                                                 "nuke.delete(nuke.toNode('%s'))\n\t"
                                         "nuke.executeInMainThread(status, args=True)\n\t"
                                         "nuke.executeInMainThread(nuke.execute, args=('%s', nuke.FrameRanges([%s])))\n\t"
                                         "nuke.executeInMainThread(status, args=False)\n"
                                     "thread.start_new_thread(writer,())''')")%m_node->m_node_name
                                                                              %m_stamp
                                                                              %writeNodeName
                                                                              %writeNodeName
                                                                              %writeNodeName
                                                                              %frames).str();
        script_command(cmd.c_str(), true, false);
        script_unlock();
    }
    cleanByLimit();
}

void Aton::importCmd(bool all)
{
    std::vector<std::string> captures = getCaptures();
    if (!captures.empty())
    {
        using namespace boost::filesystem;
        path filepath(m_path);
        path dir = filepath.parent_path();
        
        // Reverse iterating through vector
        std::vector<std::string>::reverse_iterator it;
        for(it = captures.rbegin(); it != captures.rend(); ++it)
        {
            if (all == false && it != captures.rbegin())
                continue;

            path file = *it;
            path path = dir / file;
            std::string str_path = path.string();
            boost::replace_all(str_path, "\\", "/");

            std::string cmd; // Our python command buffer
            cmd = (boost::format("exec('''readNodes = nuke.allNodes('Read')\n"
                                          "exist = False\n"
                                          "if len(readNodes)>0:\n\t"
                                              "for i in readNodes:\n\t\t"
                                                  "if '%s' == i['file'].value():\n\t\t\t"
                                                      "exist = True\n"
                                           "if exist != True:\n\t"
                                              "nuke.nodes.Read(file='%s')''')")%str_path
                                                                               %str_path ).str();
            script_command(cmd.c_str(), true, false);
            script_unlock();
        }
    }
}

void Aton::liveCameraToogle()
{
    // Our python command buffer
    std::string cmd, focalExpr;
    
    if (m_live_camera)
    {
        // Set Focal Length
        focalExpr = (boost::format("%s.cam_fov_knob!=0?(haperture/(2*tan(pi*%s.cam_fov_knob/360))):this")%m_node->m_node_name
                                                                                                         %m_node->m_node_name).str();
        // Set Matrix
        cmd = (boost::format("exec('''cam = nuke.nodes.Camera(name='%s_Camera')\n"
                             "cam['haperture'].setValue(36)\n"
                             "cam['vaperture'].setValue(24)\n"
                             "cam['focal'].setExpression('%s')\n"
                             "cam['useMatrix'].setValue(True)\n"
                             "for i in range(0, 16):\n\t"
                                 "cam['matrix'].setExpression('%s.cM'+str(i), i)''')")%m_node->m_node_name
                                                                                      %focalExpr
                                                                                      %m_node->m_node_name).str();
    }
    else
        cmd = (boost::format("nuke.delete(nuke.toNode('%s_Camera'))")%m_node->m_node_name).str();

    script_command(cmd.c_str(), true, false);
    script_unlock();
}

void Aton::setStatus(const long long& progress,
                     const long long& ram,
                     const long long& p_ram,
                     const int& time,
                     const double& frame,
                     const char* version)
{
    const int hour = time / 3600000;
    const int minute = (time % 3600000) / 60000;
    const int second = ((time % 3600000) % 60000) / 1000;
    const size_t f_count = m_node->m_framebuffers.size();

    std::string str_status = (boost::format("Arnold: %s | "
                                            "Memory: %sMB / %sMB | "
                                            "Time: %02ih:%02im:%02is | "
                                            "Frame: %04i (%s) | "
                                            "Progress: %s%%")%version%ram%p_ram
                                                             %hour%minute%second
                                                             %frame%f_count%progress).str();
    knob("status_knob")->set_text(str_status.c_str());
}

void Aton::setCameraKnobs(const float& fov, const Matrix4& matrix)
{
    std::string knob_value = (boost::format("%s")%fov).str();
    knob("cam_fov_knob")->set_text(knob_value.c_str());
    
    int k_index = 0;
    for (int i=0; i<4; i++)
    {
        for (int j=0; j<4; j++)
        {
            const float value_m = *(matrix[i]+j);
            knob_value = (boost::format("%s")%value_m).str();
            std::string knob_name = (boost::format("cM%s")%k_index).str();
            knob(knob_name.c_str())->set_text(knob_value.c_str());
            k_index++;
        }
    }
}

void Aton::setCurrentFrame(const double& frame)
{
    // Set Current Frame and update the UI
    if (frame != uiContext().frame())
    {
        OutputContext ctxt = outputContext();
        ReadGuard lock(m_mutex);
        ctxt.setFrame(frame);
        gotoContext(ctxt, true);
    }
}

// Nuke node builder
static Iop* constructor(Node* node){ return new Aton(node); }
const Iop::Description Aton::desc(CLASS, 0, constructor);
