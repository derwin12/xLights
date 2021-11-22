#pragma once

/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/smeighan/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/smeighan/xLights/blob/master/License.txt
 **************************************************************/

#include "wx/glcanvas.h"
#include "../xlGraphicsContext.h"
#include "DrawGLUtils.h"

class wxImage;


class xlGLCanvas
    : public wxGLCanvas
{
    public:
        xlGLCanvas(wxWindow* parent,
                   wxWindowID id,
                   const wxPoint &pos=wxDefaultPosition,
                   const wxSize &size=wxDefaultSize,
                   long style=0,
                   const wxString &name=wxPanelNameStr,
                   bool only2d = true);
        xlGLCanvas(wxWindow* parent,
                   const wxGLAttributes& dispAttrs,
                   const wxString &name = wxPanelNameStr);
        virtual ~xlGLCanvas();

        void SetCurrentGLContext();
        int GetCreatedVersion() const {
            return _ver;
        }

        int getWidth() const { return mWindowWidth; }
        int getHeight() const { return mWindowHeight; }

        double translateToBacking(double x) const;
        double mapLogicalToAbsolute(double x) const;


        void DisplayWarning(const wxString &msg);

		  // Grab a copy of the front buffer (at window dimensions by default); it's the
		  // caller's responsibility to delete the image when done with it
		  wxImage *GrabImage( wxSize size = wxSize(0,0) );

		  virtual void render( const wxSize& = wxSize(0,0) ) {};

		  class CaptureHelper
		  {
		  public:
			  // note: width & height without content-scale factor
			  CaptureHelper(int i_width, int i_height, double i_contentScaleFactor) : width(i_width), height(i_height), contentScaleFactor(i_contentScaleFactor), tmpBuf(nullptr) {};
			  virtual ~CaptureHelper();

			  bool ToRGB(unsigned char *buf, unsigned int bufSize, bool padToEvenDims=false);
		  protected:
			  const int width;
			  const int height;
			  const double contentScaleFactor;
			  unsigned char *tmpBuf;
		  };
    
        int GetZDepth() const { return m_zDepth;}
        static wxGLContext *GetSharedContext() { return m_sharedContext; }

        virtual xlColor ClearBackgroundColor() const { return xlBLACK; }
        virtual bool drawingUsingLogicalSize() const { return true; }


        virtual void PrepareCanvas();
        virtual xlGraphicsContext* PrepareContextForDrawing();
        virtual xlGraphicsContext* PrepareContextForDrawing(const xlColor &bg);
        virtual void FinishDrawing(xlGraphicsContext* ctx);
        void Resized(wxSizeEvent& evt);

    protected:
      	DECLARE_EVENT_TABLE()

        size_t mWindowWidth;
        size_t mWindowHeight;
        int mWindowResized;
        bool mIsInitialized;

        virtual void InitializeGLCanvas() { mIsInitialized = true; };
        virtual void InitializeGLContext() {}
        void prepare2DViewport(int topleft_x, int topleft_y, int bottomrigth_x, int bottomrigth_y);
        void prepare3DViewport(int topleft_x, int topleft_y, int bottomrigth_x, int bottomrigth_y);
        void OnEraseBackGround(wxEraseEvent& event) {};

        void CreateGLContext();

        DrawGLUtils::xlGLCacheInfo *cache = nullptr;

    private:
        int _ver = 0;
        bool is3d = false;
        wxString _name;
        wxGLContext* m_context = nullptr;
        bool m_coreProfile = false;
        int  m_zDepth = 0;
    
        static wxGLContext *m_sharedContext;
};