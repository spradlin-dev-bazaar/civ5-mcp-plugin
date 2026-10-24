// Built without the game's precompiled header; see McpMapDraw.h.
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include "McpMapDraw.h"
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
	using namespace Gdiplus;

	// Pointy-top hexes in rows, odd rows half a hex east, as the game lays them out. At scale 1
	// a hex is 64 pixels tall, with its coordinates across the top, a unit row in the middle
	// and what is on the ground along the bottom; the drawing scales up to BASE_SCALE for legibility
	// and back down only when a wide picture would exceed the longest edge a model is sent.
	const float BASE_SCALE = 1.5f;
	const int MAX_WIDTH = 1560;

	struct Layout
	{
		float s, hexR, hexW, rowH, margin, rowLabels, colLabels, legendRow;
		Layout(float scale)
			: s(scale), hexR(32 * scale), hexW(1.7320508f * 32 * scale), rowH(1.5f * 32 * scale),
			  margin(8 * scale), rowLabels(30 * scale), colLabels(16 * scale), legendRow(22 * scale) {}
		int GridW(int cols) const { return (int)(hexW * (cols + 0.5f) + 2 * margin + rowLabels); }
	};

	bool s_gdiplusTried = false, s_gdiplusStarted = false;
	ULONG_PTR s_gdiplusToken = 0;

	bool EnsureGdiplus()
	{
		if(!s_gdiplusTried)
		{
			s_gdiplusTried = true;
			GdiplusStartupInput input;
			s_gdiplusStarted = GdiplusStartup(&s_gdiplusToken, &input, NULL) == Ok;
		}
		return s_gdiplusStarted;
	}

	Color Rgb(unsigned rgb)
	{
		return Color(255, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
	}

	// Territory shows as the terrain color leaning toward the owner's, as the strategic view does.
	unsigned Tinted(unsigned terrain, unsigned owner)
	{
		unsigned out = 0;
		for(int shift = 0; shift <= 16; shift += 8)
		{
			int t = (terrain >> shift) & 255, o = (owner >> shift) & 255;
			out |= (unsigned)((t * 75 + o * 25) / 100) << shift;
		}
		return out;
	}

	Color TextOn(unsigned rgb)
	{
		int lum = (((rgb >> 16) & 255) * 299 + ((rgb >> 8) & 255) * 587 + (rgb & 255) * 114) / 1000;
		return lum > 140 ? Color(255, 0, 0, 0) : Color(255, 255, 255, 255);
	}

	std::wstring Wide(const std::string& s)
	{
		int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
		std::wstring w(n, L' ');
		if(n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
		return w;
	}

	// The six corners at radius r, from the top clockwise: the edge from corner k to k+1 faces, in
	// order, northeast, east, southeast, southwest, west, northwest, matching the road and border arrays.
	void Corners(float cx, float cy, float r, PointF out[6])
	{
		for(int k = 0; k < 6; ++k)
		{
			float a = (90.0f - 60.0f * k) * 3.14159265f / 180.0f;
			out[k] = PointF(cx + r * cos(a), cy - r * sin(a));
		}
	}

	// Text with a thin outline in the opposite tone, so it stays readable over the territory
	// dots and the fog hatching: light around dark lettering, dark around light.
	void DrawCentered(Graphics& g, const std::string& text, const Font& font, const Color& color, float x, float y, float outline = 1.0f)
	{
		StringFormat fmt;
		fmt.SetAlignment(StringAlignmentCenter);
		fmt.SetLineAlignment(StringAlignmentCenter);
		std::wstring w = Wide(text);
		if(outline > 0)
		{
			bool bDark = color.GetR() + color.GetG() + color.GetB() < 384;
			SolidBrush halo(bDark ? Color(230, 245, 245, 240) : Color(230, 14, 14, 18));
			for(int dy = -1; dy <= 1; ++dy)
				for(int dx = -1; dx <= 1; ++dx)
					if(dx || dy) g.DrawString(w.c_str(), (INT)w.size(), &font, PointF(x + dx * outline, y + dy * outline), &fmt, &halo);
		}
		SolidBrush brush(color);
		g.DrawString(w.c_str(), (INT)w.size(), &font, PointF(x, y), &fmt, &brush);
	}

	bool PngEncoderClsid(CLSID& out)
	{
		UINT num = 0, size = 0;
		if(GetImageEncodersSize(&num, &size) != Ok || size == 0) return false;
		std::vector<BYTE> buf(size);
		ImageCodecInfo* codecs = (ImageCodecInfo*)&buf[0];
		if(GetImageEncoders(num, size, codecs) != Ok) return false;
		for(UINT i = 0; i < num; ++i)
			if(wcscmp(codecs[i].MimeType, L"image/png") == 0) { out = codecs[i].Clsid; return true; }
		return false;
	}

	std::string ToPng(Bitmap& bmp)
	{
		CLSID clsid;
		if(!PngEncoderClsid(clsid)) return "";
		IStream* stream = NULL;
		if(FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream))) return "";
		std::string bytes;
		if(bmp.Save(stream, &clsid, NULL) == Ok)
		{
			HGLOBAL h = NULL;
			STATSTG st;
			if(SUCCEEDED(GetHGlobalFromStream(stream, &h)) && SUCCEEDED(stream->Stat(&st, STATFLAG_NONAME)))
			{
				SIZE_T n = (SIZE_T)st.cbSize.LowPart;
				if(n > GlobalSize(h)) n = GlobalSize(h);
				const char* data = (const char*)GlobalLock(h);
				if(data) { bytes.assign(data, n); GlobalUnlock(h); }
			}
		}
		stream->Release();
		return bytes;
	}
}

std::string McpDrawHexMapPng(const std::vector<McpMapHex>& hexes, const std::vector<McpMapLegendEntry>& legend, const std::string& footer, int x0, int y1, int cols, int rows, int wrapWidth, std::string& err)
{
	if(!EnsureGdiplus()) { err = "the picture could not be drawn: Windows drawing is unavailable"; return ""; }
	float scale = BASE_SCALE;
	if(Layout(scale).GridW(cols) > MAX_WIDTH) scale = BASE_SCALE * MAX_WIDTH / Layout(scale).GridW(cols);
	const Layout L(scale);
	const float s = L.s;
	int gridW = L.GridW(cols);
	int gridH = (int)(L.rowH * (rows - 1) + 2 * L.hexR + 2 * L.margin + L.colLabels);
	int legendRows = ((int)legend.size() + 2) / 3;
	int height = (int)(gridH + (legendRows + 1) * L.legendRow + L.margin);

	Bitmap bmp(gridW, height, PixelFormat24bppRGB);
	Graphics g(&bmp);
	g.SetSmoothingMode(SmoothingModeAntiAlias);
	// Crisp, unsmoothed text: smoothed text this small reads as a blur.
	g.SetTextRenderingHint(TextRenderingHintSingleBitPerPixelGridFit);
	g.Clear(Color(255, 30, 30, 34));

	FontFamily family(L"Verdana");
	Font labelFont(&family, 11 * s, FontStyleRegular, UnitPixel);
	Font coordFont(&family, 10 * s, FontStyleBold, UnitPixel);
	Font unitFont(&family, 10 * s, FontStyleBold, UnitPixel);
	Font cityFont(&family, 11 * s, FontStyleBold, UnitPixel);
	Font centerFont(&family, 13 * s, FontStyleBold, UnitPixel);
	Font legendFont(&family, 12 * s, FontStyleRegular, UnitPixel);
	Pen gridPen(Color(255, 10, 10, 12), 2 * s);
	Pen riverOutline(Color(255, 0, 50, 80), 6 * s);
	Pen riverPen(Color(255, 0, 215, 255), 3 * s);
	Pen roadOutline(Color(255, 235, 215, 170), 6 * s);
	Pen roadPen(Color(255, 90, 55, 25), 3 * s);
	HatchBrush fogBrush(HatchStyleWideUpwardDiagonal, Color(150, 20, 20, 24), Color(60, 20, 20, 24));
	Color axisColor(255, 170, 170, 180);

	std::vector<PointF> centers(hexes.size());
	for(size_t i = 0; i < hexes.size(); ++i)
	{
		const McpMapHex& h = hexes[i];
		centers[i] = PointF(L.margin + L.rowLabels + L.hexW * (h.x - x0 + 0.5f) + ((h.y & 1) ? L.hexW / 2 : 0.0f), L.margin + L.colLabels + L.hexR + L.rowH * (y1 - h.y));
	}
	{
		SolidBrush grey(axisColor);
		char label[16];
		for(int r = 0; r < rows; ++r)
		{
			sprintf_s(label, "y%d", y1 - r);
			std::wstring w = Wide(label);
			g.DrawString(w.c_str(), (INT)w.size(), &coordFont, PointF(L.margin, L.margin + L.colLabels + L.hexR + L.rowH * r - 7 * s), &grey);
		}
		// The top row's x values, above its hexes; the next row down sits half a hex to the side.
		for(int c = 0; c < cols; ++c)
		{
			int x = x0 + c;
			if(wrapWidth > 0) x = (x % wrapWidth + wrapWidth) % wrapWidth;
			sprintf_s(label, "x%d", x);
			DrawCentered(g, label, coordFont, axisColor, L.margin + L.rowLabels + L.hexW * (c + 0.5f) + ((y1 & 1) ? L.hexW / 2 : 0.0f), L.margin + 6 * s, 0);
		}
	}

	// Ground, then edges, then fog, then labels and what stands on the tiles, so the words stay readable.
	for(size_t i = 0; i < hexes.size(); ++i)
	{
		const McpMapHex& h = hexes[i];
		PointF c[6];
		Corners(centers[i].X, centers[i].Y, L.hexR, c);
		SolidBrush fill(h.unexplored ? Color(255, 52, 52, 60) : Rgb(h.owned ? Tinted(h.fill, h.borderColor) : h.fill));
		g.FillPolygon(&fill, c, 6);
		if(h.owned)
		{
			// Territory also carries a grid of dots in the owner's color, which reads at a glance
			// and leaves the terrain color between the dots recognizable.
			GraphicsPath path;
			path.AddPolygon(c, 6);
			g.SetClip(&path);
			SolidBrush dot(Rgb(h.borderColor));
			float step = 11 * s, r = 2.2f * s;
			int row = 0;
			for(float gy = centers[i].Y - L.hexR; gy <= centers[i].Y + L.hexR; gy += step, ++row)
				for(float gx = centers[i].X - L.hexW / 2 + (row & 1 ? step / 2 : 0); gx <= centers[i].X + L.hexW / 2; gx += step)
					g.FillEllipse(&dot, gx - r, gy - r, 2 * r, 2 * r);
			g.ResetClip();
		}
		g.DrawPolygon(&gridPen, c, 6);
	}
	for(size_t i = 0; i < hexes.size(); ++i)
	{
		const McpMapHex& h = hexes[i];
		if(h.unexplored) continue;
		PointF c[6], inner[6];
		Corners(centers[i].X, centers[i].Y, L.hexR, c);
		Corners(centers[i].X, centers[i].Y, L.hexR - 5 * s, inner);
		for(int pass = 0; pass < 2; ++pass)
			for(int k = 0; k < 6; ++k)
			{
				if(!h.road[k]) continue;
				PointF mid((c[k].X + c[(k + 1) % 6].X) / 2, (c[k].Y + c[(k + 1) % 6].Y) / 2);
				g.DrawLine(pass == 0 ? &roadOutline : &roadPen, centers[i], mid);
			}
		if(h.hasRoad)
		{
			SolidBrush dot(Color(255, 90, 55, 25));
			g.FillEllipse(&dot, centers[i].X - 4 * s, centers[i].Y - 4 * s, 8 * s, 8 * s);
		}
		// Rivers run on the shared edge; borders sit just inside the owner's tile, so the two
		// never share a line.
		for(int pass = 0; pass < 2; ++pass)
		{
			Pen* pen = pass == 0 ? &riverOutline : &riverPen;
			if(h.riverE) g.DrawLine(pen, c[1], c[2]);
			if(h.riverSE) g.DrawLine(pen, c[2], c[3]);
			if(h.riverSW) g.DrawLine(pen, c[3], c[4]);
		}
	}
	for(size_t i = 0; i < hexes.size(); ++i)
	{
		if(!hexes[i].fogged) continue;
		PointF c[6];
		Corners(centers[i].X, centers[i].Y, L.hexR, c);
		g.FillPolygon(&fogBrush, c, 6);
	}
	for(size_t i = 0; i < hexes.size(); ++i)
	{
		const McpMapHex& h = hexes[i];
		if(h.unexplored) continue;
		PointF inner[6];
		Corners(centers[i].X, centers[i].Y, L.hexR - 5 * s, inner);
		Pen border(Rgb(h.borderColor), 5 * s);
		for(int k = 0; k < 6; ++k)
			if(h.border[k]) g.DrawLine(&border, inner[k], inner[(k + 1) % 6]);
	}
	for(size_t i = 0; i < hexes.size(); ++i)
	{
		const McpMapHex& h = hexes[i];
		float px = centers[i].X, py = centers[i].Y;
		if(h.unexplored)
		{
			DrawCentered(g, "?", centerFont, Color(255, 120, 120, 130), px, py, 0);
			continue;
		}
		Color ink = h.fogged ? Color(255, 235, 235, 235) : TextOn(h.fill);
		// Three centered rows inside the hex's flat-sided middle, so every word is plainly this
		// hex's own: coordinates on top; the city, units or mountain mark in the middle; what is
		// on the ground at the bottom (resource, feature with h for hills, improvement).
		std::string top = h.coords;
		int extra = h.extraUnits + (int)h.units.size() - (h.units.size() > 2 ? 2 : (int)h.units.size());
		if(!h.city && extra > 0)
		{
			char more[16];
			sprintf_s(more, " +%d", extra);
			top += more;
		}
		DrawCentered(g, top, coordFont, ink, px, py - 16 * s);
		std::string ground = h.topLeft;
		if(!h.topRight.empty()) ground += (ground.empty() ? "" : " ") + h.topRight;
		if(!h.bottomLeft.empty() && ground.size() + h.bottomLeft.size() <= 11) ground += (ground.empty() ? "" : " ") + h.bottomLeft;
		if(!ground.empty()) DrawCentered(g, ground, labelFont, ink, px, py + 14 * s);
		if(h.city)
		{
			PointF c[6];
			Corners(px, py, L.hexR - 1 * s, c);
			Pen cityHex(Color(255, 255, 255, 255), 2 * s);
			g.DrawPolygon(&cityHex, c, 6);
			SolidBrush box(Rgb(h.cityColor));
			Pen boxEdge(Color(255, 0, 0, 0), 1 * s);
			RectF r(px - 29 * s, py - 9 * s, 58 * s, 18 * s);
			g.FillRectangle(&box, r);
			g.DrawRectangle(&boxEdge, r);
			DrawCentered(g, h.cityLabel, cityFont, TextOn(h.cityColor), px, py, 0);
		}
		else if(!h.units.empty())
		{
			int shown = (int)h.units.size();
			if(shown > 2) shown = 2;
			for(int k = 0; k < shown; ++k)
			{
				const McpMapUnit& u = h.units[k];
				float ux = px + (k - (shown - 1) / 2.0f) * 23 * s;
				SolidBrush fill(Rgb(u.fill));
				Pen edge(Rgb(u.ring), 4 * s);
				g.FillEllipse(&fill, ux - 10 * s, py - 10.5f * s, 20 * s, 20 * s);
				g.DrawEllipse(&edge, ux - 10 * s, py - 10.5f * s, 20 * s, 20 * s);
				DrawCentered(g, u.label, unitFont, TextOn(u.fill), ux, py - 0.5f * s, 0);
			}
		}
		else if(!h.centerLabel.empty())
		{
			DrawCentered(g, h.centerLabel, centerFont, ink, px, py);
		}
	}

	float ly = (float)gridH;
	SolidBrush white(Color(255, 235, 235, 235));
	for(size_t k = 0; k < legend.size(); ++k)
	{
		float lx = L.margin + (k % 3) * (gridW - 2 * L.margin) / 3.0f;
		float row = ly + (k / 3) * L.legendRow;
		SolidBrush sw(Rgb(legend[k].color));
		g.FillRectangle(&sw, lx, row + 4 * s, 14 * s, 14 * s);
		std::wstring w = Wide(legend[k].name);
		g.DrawString(w.c_str(), (INT)w.size(), &legendFont, PointF(lx + 19 * s, row + 3 * s), &white);
	}
	{
		SolidBrush grey(Color(255, 200, 200, 200));
		std::wstring w = Wide(footer);
		g.DrawString(w.c_str(), (INT)w.size(), &legendFont, PointF(L.margin, ly + legendRows * L.legendRow + 3 * s), &grey);
	}

	g.Flush(FlushIntentionSync);
	std::string png = ToPng(bmp);
	if(png.empty()) err = "the picture could not be encoded";
	return png;
}

void McpSaveMapCopy(const std::string& png)
{
	char path[MAX_PATH];
	if(FAILED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, path))) return;
	std::string file = std::string(path) + "\\My Games\\Sid Meier's Civilization 5\\Logs\\map_image_last.png";
	FILE* f = NULL;
	if(fopen_s(&f, file.c_str(), "wb") == 0 && f) { fwrite(png.data(), 1, png.size(), f); fclose(f); }
}

void McpShutdownMapDraw()
{
	if(s_gdiplusStarted) GdiplusShutdown(s_gdiplusToken);
	s_gdiplusStarted = false;
}
