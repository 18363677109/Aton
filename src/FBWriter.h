/*
Copyright (c) 2016,
Dan Bethell, Johannes Saam, Vahan Sosoyan, Brian Scherbinski.
All rights reserved. See COPYING.txt for more details.
*/

#ifndef FBWriter_h
#define FBWriter_h

#include "Aton.h"

// Our FrameBuffer writer thread
static void FBWriter(unsigned index, unsigned nthreads, void* data)
{
    bool killThread = false;
    std::vector<std::string> active_aovs;

    Aton* node = reinterpret_cast<Aton*> (data);

    while (!killThread)
    {
        // Accept incoming connections!
        node->m_server.accept();

        // Our incoming data object
        Data d;

        // Frame index in FrameBuffers
        int f_index = 0;
        // For progress percentage
        long long progress, regionArea = 0;
        
        // Time to reset per every IPR iteration
        static int _active_time, delta_time = 0;
        
        // Loop over incoming data
        while ((d.type() == 2 || d.type() == 9) == false)
        {
            // Listen for some data
            try
            {
                d = node->m_server.listen();
            }
            catch( ... )
            {
                break;
            }

            // Handle the data we received
            switch (d.type())
            {
                case 0: // Open a new image
                {
                    // Copy data from d
                    const int& _xres = d.xres();
                    const int& _yres = d.yres();
                    const int& _version = d.version();
                    const float& _fov = d.camFov();
                    const long long& _area = d.rArea();
                    const Matrix4& _matrix = Matrix4(&d.camMatrix()[0]);
                    const double& _frame = static_cast<double>(d.currentFrame());
                    
                    // Get image area to calculate the progress
                    regionArea = _area;
                    
                    // Get delta time per IPR iteration
                    delta_time = _active_time;
                    
                    // Set current frame
                    node->m_current_frame = _frame;
                    
                    std::vector<double>& m_frs = node->m_frames;
                    std::vector<FrameBuffer>& m_fbs = node->m_framebuffers;

                    // Create FrameBuffer
                    if (node->m_multiframes)
                    {
                        if (std::find(m_frs.begin(), m_frs.end(), _frame) == m_frs.end())
                        {
                            FrameBuffer fB(_frame, _xres, _yres);
                            if (!m_frs.empty())
                                fB = m_fbs.back();
                            WriteGuard lock(node->m_mutex);
                            m_frs.push_back(_frame);
                            m_fbs.push_back(fB);
                        }
                    }
                    else
                    {
                        FrameBuffer fB(_frame, _xres, _yres);
                        if (!node->m_frames.empty())
                        {
                            f_index = node->getFrameIndex(node->m_frames, node->m_current_frame);
                            fB = m_fbs[f_index];
                        }
                        WriteGuard lock(node->m_mutex);
                        m_frs = std::vector<double>();
                        m_fbs = std::vector<FrameBuffer>();
                        m_frs.push_back(_frame);
                        m_fbs.push_back(fB);
                    }
                    
                    // Get current FrameBuffer
                    f_index = node->getFrameIndex(node->m_frames, _frame);
                    FrameBuffer& fB = m_fbs[f_index];
                    
                    // Reset Frame and Buffers if changed
                    if (!fB.empty() && !active_aovs.empty())
                    {
                        if (fB.isFrameChanged(_frame))
                        {
                            WriteGuard lock(node->m_mutex);
                            fB.setFrame(_frame);
                        }
                        if(fB.isAovsChanged(active_aovs))
                        {
                            WriteGuard lock(node->m_mutex);
                            fB.resize(1);
                            fB.ready(false);
                            node->resetChannels(node->m_channels);
                        }
                    }
                    
                    // Setting Camera
                    if (fB.isCameraChanged(_fov, _matrix))
                    {
                        WriteGuard lock(node->m_mutex);
                        fB.setCamera(_fov, _matrix);
                        node->setCameraKnobs(fB.getCameraFov(),
                                             fB.getCameraMatrix());
                    }

                    // Set Arnold Core version
                    if (fB.getAiVersionInt() != _version)
                        fB.setAiVersion(_version);
                    
                    // Reset active AOVs
                    if(!active_aovs.empty())
                        active_aovs.clear();
                    break;
                }
                case 1: // Write image data
                {
                    // Get frame buffer
                    FrameBuffer& fB = node->m_framebuffers[f_index];
                    const char* _aov_name = d.aovName();
                    const int& _xres = d.xres();
                    const int& _yres = d.yres();

                    if(fB.isResolutionChanged(_xres, _yres))
                    {
                        WriteGuard lock(node->m_mutex);
                        fB.setResolution(_xres, _yres);
                    }

                    // Get active aov names
                    if(std::find(active_aovs.begin(),
                                 active_aovs.end(),
                                 _aov_name) == active_aovs.end())
                    {
                        if (node->m_enable_aovs || active_aovs.empty())
                            active_aovs.push_back(_aov_name);
                        else if (active_aovs.size() > 1)
                            active_aovs.resize(1);
                    }
                    
                    // Skip non RGBA buckets if AOVs are disabled
                    if (node->m_enable_aovs || active_aovs[0] == _aov_name)
                    {
                        // Get data from d
                        const int& _x = d.bucket_xo();
                        const int& _y = d.bucket_yo();
                        const int& _width = d.bucket_size_x();
                        const int& _height = d.bucket_size_y();
                        const int& _spp = d.spp();
                        const long long& _ram = d.ram();
                        const int& _time = d.time();

                        // Set active time
                        _active_time = _time;
                        
                        // Get framebuffer width and height
                        const int& w = fB.getWidth();
                        const int& h = fB.getHeight();

                        // Adding buffer
                        node->m_mutex.writeLock();
                        if(!fB.isBufferExist(_aov_name) && (node->m_enable_aovs || fB.empty()))
                            fB.addBuffer(_aov_name, _spp);
                        else
                            fB.ready(true);
                        
                        // Get buffer index
                        const int b = fB.getBufferIndex(_aov_name);
                    
                        // Writing to buffer
                        int x, y, c, xpos, ypos, offset;
                        for (x = 0; x < _width; ++x)
                        {
                            for (y = 0; y < _height; ++y)
                            {
                                offset = (_width * y * _spp) + (x * _spp);
                                for (c = 0; c < _spp; ++c)
                                {
                                    xpos = x + _x;
                                    ypos = h - (y + _y + 1);
                                    const float& _pix = d.pixel(offset + c);
                                    fB.setBufferPix(b, xpos, ypos, _spp, c, _pix);
                                }
                            }
                        }
                        node->m_mutex.unlock();
                        
                        // Update only on first aov
                        if(!node->m_capturing && fB.isFirstBufferName(_aov_name))
                        {
                            // Calculate the progress percentage
                            regionArea -= (_width*_height);
                            progress = 100 - (regionArea * 100) / (w * h);

                            // Set status parameters
                            node->m_mutex.writeLock();
                            fB.setProgress(progress);
                            fB.setRAM(_ram);
                            fB.setTime(_time, delta_time);
                            node->m_mutex.unlock();
                            
                            // Update the image
                            const Box box = Box(_x, h - _y - _height, _x + _width, h - _y);
                            node->setCurrentFrame(node->m_current_frame);
                            node->flagForUpdate(box);
                        }
                    }
                    d.dealloc();
                    break;
                }
                case 2: // Close image
                {
                    break;
                }
                case 9: // This is sent when the parent process want to kill
                        // the listening thread
                {
                    killThread = true;
                    break;
                }
            }
        }
    }
}

#endif /* FBWriter_h */
