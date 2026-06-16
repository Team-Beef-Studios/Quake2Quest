/*
 * Copyright (C) 1997-2001 Id Software, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * =======================================================================
 *
 * This file implements the 2D stuff. For example the HUD and the
 * networkgraph.
 *
 * =======================================================================
 */

#include <stdbool.h>
#include "header/client.h"
#include "../../../Quake2VR/VrCvars.h"

float scr_con_current; /* aproaches scr_conlines at scr_conspeed */
float scr_conlines; /* 0.0 to 1.0 lines of console to display */

qboolean scr_initialized; /* ready to draw */

int scr_draw_loading;

vrect_t scr_vrect; /* position of render window on screen */

cvar_t *scr_viewsize;
cvar_t *scr_conspeed;
cvar_t *scr_centertime;
cvar_t *scr_showturtle;
cvar_t *scr_showpause;

cvar_t *scr_netgraph;
cvar_t *scr_timegraph;
cvar_t *scr_debuggraph;
cvar_t *scr_graphheight;
cvar_t *scr_graphscale;
cvar_t *scr_graphshift;
cvar_t *scr_drawall;

cvar_t *r_hudscale; /* named for consistency with R1Q2 */
cvar_t *r_consolescale;
cvar_t *r_menuscale;

typedef struct
{
	int x1, y1, x2, y2;
} dirty_t;

dirty_t scr_dirty, scr_old_dirty[2];

char crosshair_pic[MAX_QPATH];
int crosshair_width, crosshair_height;

extern cvar_t *cl_showfps;
extern cvar_t *crosshair_scale;

void SCR_TimeRefresh_f(void);
void SCR_Loading_f(void);

const wheel_icon_t weaponIcons[11] = {
        {
                "w_blaster",
                7,
                "Blaster",
                NULL,
                0,
                0,
                -160
        },
        {
                "w_shotgun",
                8,
                "Shotgun",
                "shells",
                18,
                86,
                -134
        },
        {
                "w_sshotgun",
                9,
                "Super Shotgun",
                "shells",
                18,
                145,
                -66
        },
        {
                "w_machinegun",
                10,
                "Machinegun",
                "bullets",
                19,
                158,
                22
        },
        {
                "w_chaingun",
                11,
                "Chaingun",
                "bullets",
                19,
                120,
                104
        },
        {
                "w_grenades",
                12,
                "Grenades",
                "grenades",
                12,
                45,
                153
        },
        {
                "w_glauncher",
                13,
                "Grenade Launcher",
                "grenades",
                12,
                -45,
                153
        },
        {
                "w_rlauncher",
                14,
                "Rocket Launcher",
                "rockets",
                21,
                -120,
                104
        },
        {
                "w_hyperblaster",
                15,
                "HyperBlaster",
                "cells",
                20,
                -158,
                22
        },
        {
                "w_railgun",
                16,
                "Railgun",
                "slugs",
                22,
                -145,
                -66
        },
        {
                "w_bfg",
                17,
                "BFG10K",
                "cells",
                20,
                -86,
                -134
        }
};

const wheel_icon_t itemIcons[6] = {
        {
                "p_silencer",
                25,
                "Silencer",
                NULL,
                0,
                0,
                -160
        },
        {
                "p_rebreather",
                26,
                "Rebreather",
                NULL,
                0,
                138,
                -80
        },
        {
                "p_envirosuit",
                27,
                "Environment Suit",
                NULL,
                0,
                138,
                80
        },
        {
                "i_powershield",
                6,
                "Power Shield",
                NULL,
                0,
                0,
                160
        },
        {
                "p_quad",
                23,
                "Quad Damage",
                NULL,
                0,
                -138,
                80
        },
        {
                "p_invulnerability",
                24,
                "Invulnerability",
                NULL,
                0,
                -138,
                -80
        }
};

/*
 * A new packet was just parsed
 */
void
CL_AddNetgraph(void)
{
	int i;
	int in;
	int ping;

	/* if using the debuggraph for something
	   else, don't add the net lines */
	if (scr_debuggraph->value || scr_timegraph->value)
	{
		return;
	}

	for (i = 0; i < cls.netchan.dropped; i++)
	{
		SCR_DebugGraph(30, 0x40);
	}

	for (i = 0; i < cl.surpressCount; i++)
	{
		SCR_DebugGraph(30, 0xdf);
	}

	/* see what the latency was on this packet */
	in = cls.netchan.incoming_acknowledged & (CMD_BACKUP - 1);
	ping = cls.realtime - cl.cmd_time[in];
	ping /= 30;

	if (ping > 30)
	{
		ping = 30;
	}

	SCR_DebugGraph((float)ping, 0xd0);
}

typedef struct
{
	float value;
	int color;
} graphsamp_t;

static int current;
static graphsamp_t values[2024];

void
SCR_DebugGraph(float value, int color)
{
	values[current & 2023].value = value;
	values[current & 2023].color = color;
	current++;
}

void
SCR_DrawDebugGraph(void)
{
	int a, x, y, w, i, h;
	float v;
	int color;

	/* draw the graph */
	w = scr_vrect.width;

	x = scr_vrect.x;
	y = scr_vrect.y + scr_vrect.height;
	Draw_Fill(x, y - scr_graphheight->value,
			w, scr_graphheight->value, 8);

	for (a = 0; a < w; a++)
	{
		i = (current - 1 - a + 1024) & 1023;
		v = values[i].value;
		color = values[i].color;
		v = v * scr_graphscale->value + scr_graphshift->value;

		if (v < 0)
		{
			v += scr_graphheight->value *
				 (1 + (int)(-v / scr_graphheight->value));
		}

		h = (int)v % (int)scr_graphheight->value;
		Draw_Fill(x + w - 1 - a, y - h, 1, h, color);
	}
}

char scr_centerstring[1024];
float scr_centertime_start; /* for slow victory printing */
float scr_centertime_off;
int scr_center_lines;
int scr_erase_center;

/*
 * Called for important messages that should stay
 * in the center of the screen for a few moments
 */
void
SCR_CenterPrint(char *str)
{
	char *s;
	char line[64];
	int i, j, l;

	Q_strlcpy(scr_centerstring, str, sizeof(scr_centerstring));
	scr_centertime_off = scr_centertime->value;
	scr_centertime_start = cl.time;

	/* count the number of lines for centering */
	scr_center_lines = 1;
	s = str;

	while (*s)
	{
		if (*s == '\n')
		{
			scr_center_lines++;
		}

		s++;
	}

	/* echo it to the console */
	Com_Printf("\n\n\35\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\37\n\n");

	s = str;

	do
	{
		/* scan the width of the line */
		for (l = 0; l < 40; l++)
		{
			if ((s[l] == '\n') || !s[l])
			{
				break;
			}
		}

		for (i = 0; i < (40 - l) / 2; i++)
		{
			line[i] = ' ';
		}

		for (j = 0; j < l; j++)
		{
			line[i++] = s[j];
		}

		line[i] = '\n';
		line[i + 1] = 0;

		Com_Printf("%s", line);

		while (*s && *s != '\n')
		{
			s++;
		}

		if (!*s)
		{
			break;
		}

		s++; /* skip the \n */
	}
	while (1);

	Com_Printf("\n\n\35\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\37\n\n");
	Con_ClearNotify();
}

/* defined further down; forward-declared so the HUD/centerprint draws above can converge for VR */
static int SCR_GetStereoHudOffsetScaled(float separation, float depthScale);
static int SCR_GetStereoHudOffset(float separation);

void
SCR_DrawCenterString(float separation)
{
	char *start;
	int l;
	int j;
	int x, y;
	int remaining;
	float scale;
    const int char_unscaled_width  = 8;
    const int char_unscaled_height = 8;
	/* centerprints (e.g. level hint messages like "crouch here") must be shifted
	 * per-eye like the rest of the HUD, otherwise they're not stereo-converged and
	 * are unreadable in VR. */
	int offset_stereo = SCR_GetStereoHudOffset(separation);

	/* the finale prints the characters one at a time */
	remaining = 9999;

	scr_erase_center = 0;
	start = scr_centerstring;
	scale = SCR_GetConsoleScale();

	if (scr_center_lines <= 4)
	{
		y = (viddef.height * 0.4) / scale;
	}

	else
	{
		y = 48 / scale;
	}

	do
	{
		/* scan the width of the line */
		for (l = 0; l < 40; l++)
		{
			if ((start[l] == '\n') || !start[l])
			{
				break;
			}
		}

		x = ((viddef.width / scale) - (l * char_unscaled_width)) / 2;
		SCR_AddDirtyPoint(x, y);

		for (j = 0; j < l; j++, x += char_unscaled_width)
		{
			Draw_CharScaled(x * scale + offset_stereo, y * scale, start[j], scale);

			if (!remaining--)
			{
				return;
			}
		}

		SCR_AddDirtyPoint(x, y + char_unscaled_height);

		y += char_unscaled_height;

		while (*start && *start != '\n')
		{
			start++;
		}

		if (!*start)
		{
			break;
		}

		start++; /* skip the \n */
	}
	while (1);
}

void
SCR_CheckDrawCenterString(float separation)
{
	scr_centertime_off -= cls.rframetime;

	if (scr_centertime_off <= 0)
	{
		return;
	}

	SCR_DrawCenterString(separation);
}

/*
 * Sets scr_vrect, the coordinates of the rendered window
 */
static void
SCR_CalcVrect(void)
{
	int size;

	/* bound viewsize */
	if (scr_viewsize->value < 40)
	{
		Cvar_Set("viewsize", "40");
	}

	if (scr_viewsize->value > 100)
	{
		Cvar_Set("viewsize", "100");
	}

	size = scr_viewsize->value;

	scr_vrect.width = viddef.width * size / 100;
	scr_vrect.height = viddef.height * size / 100;

	scr_vrect.x = (viddef.width - scr_vrect.width) / 2;
	scr_vrect.y = (viddef.height - scr_vrect.height) / 2;
}

/*
 * Keybinding command
 */
void
SCR_SizeUp_f(void)
{
	Cvar_SetValue("viewsize", (float)scr_viewsize->value + 10);
}

/*
 *Keybinding command
 */
void
SCR_SizeDown_f(void)
{
	Cvar_SetValue("viewsize", (float)scr_viewsize->value - 10);
}

/*
 * Set a specific sky and rotation speed
 */
void
SCR_Sky_f(void)
{
	float rotate;
	vec3_t axis;

	if (Cmd_Argc() < 2)
	{
		Com_Printf("Usage: sky <basename> <rotate> <axis x y z>\n");
		return;
	}

	if (Cmd_Argc() > 2)
	{
		rotate = (float)strtod(Cmd_Argv(2), (char **)NULL);
	}

	else
	{
		rotate = 0;
	}

	if (Cmd_Argc() == 6)
	{
		axis[0] = (float)strtod(Cmd_Argv(3), (char **)NULL);
		axis[1] = (float)strtod(Cmd_Argv(4), (char **)NULL);
		axis[2] = (float)strtod(Cmd_Argv(5), (char **)NULL);
	}
	else
	{
		axis[0] = 0;
		axis[1] = 0;
		axis[2] = 1;
	}

	R_SetSky(Cmd_Argv(1), rotate, axis);
}

void
SCR_Init(void)
{
	scr_viewsize = Cvar_Get("viewsize", "100", CVAR_ARCHIVE);
	scr_conspeed = Cvar_Get("scr_conspeed", "3", 0);
	scr_centertime = Cvar_Get("scr_centertime", "2.5", 0);
	scr_showturtle = Cvar_Get("scr_showturtle", "0", 0);
	scr_showpause = Cvar_Get("scr_showpause", "1", 0);
	scr_netgraph = Cvar_Get("netgraph", "0", 0);
	scr_timegraph = Cvar_Get("timegraph", "0", 0);
	scr_debuggraph = Cvar_Get("debuggraph", "0", 0);
	scr_graphheight = Cvar_Get("graphheight", "32", 0);
	scr_graphscale = Cvar_Get("graphscale", "1", 0);
	scr_graphshift = Cvar_Get("graphshift", "0", 0);
	scr_drawall = Cvar_Get("scr_drawall", "0", 0);
	r_hudscale = Cvar_Get("r_hudscale", "-1", CVAR_ARCHIVE);
	r_consolescale = Cvar_Get("r_consolescale", "-1", CVAR_ARCHIVE);
	r_menuscale = Cvar_Get("r_menuscale", "-1", CVAR_ARCHIVE);

	/* register our commands */
	Cmd_AddCommand("timerefresh", SCR_TimeRefresh_f);
	Cmd_AddCommand("loading", SCR_Loading_f);
	Cmd_AddCommand("sizeup", SCR_SizeUp_f);
	Cmd_AddCommand("sizedown", SCR_SizeDown_f);
	Cmd_AddCommand("sky", SCR_Sky_f);

	scr_initialized = true;
}

void
SCR_DrawNet(float separation)
{
	float scale = SCR_GetMenuScale();
	int offset_stereo = SCR_GetStereoHudOffset(separation);

	if (cls.netchan.outgoing_sequence - cls.netchan.incoming_acknowledged < CMD_BACKUP - 1)
	{
		return;
	}

	Draw_PicScaled(scr_vrect.x + 64 * scale + offset_stereo, scr_vrect.y, "net", scale);
}

void
SCR_DrawPause(float separation)
{
	int w, h;
	float scale = SCR_GetMenuScale();
	int offset_stereo = SCR_GetStereoHudOffset(separation);

	if (!scr_showpause->value) /* turn off for screenshots */
	{
		return;
	}

	if (!cl_paused->value)
	{
		return;
	}

	Draw_GetPicSize(&w, &h, "pause");
	Draw_PicScaled((viddef.width - w * scale) / 2 + offset_stereo, viddef.height / 2 + 8 * scale, "pause", scale);
}

/*
==============
SCR_DrawVignette
==============
*/
extern bool player_moving;

void SCR_DrawVignette (float separation)
{
	if (vr_comfort_mask->value <= 0.0f ||
		vr_comfort_mask->value > 1.0f)
	{
		return;
	}

	static float currentVLevel = 0.0f;

	if (player_moving)
	{
		if (currentVLevel <  vr_comfort_mask->value)
			currentVLevel += vr_comfort_mask->value * 0.05;
	} else{
		if (currentVLevel >  0.0f)
			currentVLevel -= vr_comfort_mask->value * 0.05;
	}

	if (currentVLevel > 0.0f &&
		currentVLevel < 1.0f)
	{
		/* The comfort mask must fill the whole eye and close in towards the eye's
		 * OPTICAL centre, which - with an asymmetric VR FOV - is not the centre of
		 * the framebuffer. Derive it from this eye's projection (the same fov tan
		 * values the renderer uses); fall back to the framebuffer centre if unknown.
		 * Note this is NOT a HUD element, so it deliberately ignores the HUD stereo
		 * offset and is sized to the full eye, not the HUD plane. */
		int eye = separation < 0 ? 0 : 1;
		float left  = Cvar_VariableValue(eye == 0 ? "gl1_openxr_fov_left_0"  : "gl1_openxr_fov_left_1");
		float right = Cvar_VariableValue(eye == 0 ? "gl1_openxr_fov_right_0" : "gl1_openxr_fov_right_1");
		float up    = Cvar_VariableValue(eye == 0 ? "gl1_openxr_fov_up_0"    : "gl1_openxr_fov_up_1");
		float down  = Cvar_VariableValue(eye == 0 ? "gl1_openxr_fov_down_0"  : "gl1_openxr_fov_down_1");
		float denomX = right - left;
		float denomY = up - down;

		/* optical centre in framebuffer pixels (tan == 0 is the view axis) */
		float cx = (fabsf(denomX) > 0.0001f) ? (viddef.width  * (-left / denomX)) : (viddef.width  * 0.5f);
		float cy = (fabsf(denomY) > 0.0001f) ? (viddef.height * (up / denomY))    : (viddef.height * 0.5f);

		/* shrink the full-eye mask towards (cx,cy); size matches the old behaviour,
		 * only the centre moves (identical when the FOV is symmetric) */
		int x = (int)(cx * currentVLevel);
		int y = (int)(cy * currentVLevel);
		int w = (int)(viddef.width  * (1.0f - currentVLevel));
		int h = (int)(viddef.height * (1.0f - currentVLevel));

		re.DrawStretchPic(x, y, w, h, "/vignette.tga");
	}
}

extern qboolean draw_item_wheel;
extern qboolean isItems;
extern vec2_t polarCursor;
extern int segment;
static float cursorFactor = 200/15; // 200 is the radius of the ring image
                                    // 15 is the same radius in VR scale

static qboolean
SCR_UsingOpenXRStereo(void)
{
	return (int)Cvar_VariableValue("gl1_stereo") == 8;
}

/*
 * Per-eye horizontal HUD offset. depthScale lets an element be placed nearer to
 * (depthScale < 1) or further from (depthScale > 1) the viewer than the rest of
 * the HUD: only the depth-parallax term is scaled, while the FOV-centering term
 * is left untouched so the element stays stereo-correct. depthScale == 1 gives
 * the normal HUD depth.
 */
static int
SCR_GetStereoHudOffsetScaled(float separation, float depthScale)
{
	/* On the flat "screen layer" (menus, console, paused, demos, cinematics) the scene
	 * is rendered once and shown to both eyes on a quad - a per-eye offset would just
	 * push the HUD off-centre with no convergence benefit. This mirrors useScreenLayer()
	 * in Q2VR_SurfaceView.c; keep the two in sync. */
	if ((cls.state != ca_connected && cls.state != ca_active) ||
		cls.key_dest != key_game ||
		cl.attractloop ||
		cl.cinematictime != 0)
	{
		return 0;
	}

	if (SCR_UsingOpenXRStereo())
	{
		int eye = separation < 0 ? 0 : 1;
		float left = Cvar_VariableValue(eye == 0 ? "gl1_openxr_fov_left_0" : "gl1_openxr_fov_left_1");
		float right = Cvar_VariableValue(eye == 0 ? "gl1_openxr_fov_right_0" : "gl1_openxr_fov_right_1");
		float width = (float)viddef.width;
		float denom = right - left;

		if (fabsf(denom) > 0.0001f)
		{
			float hud_depth = Cvar_VariableValue("vr_hud_depth");
			float hud_ipd = Cvar_VariableValue("vr_hud_ipd");
			float depth_offset;
			float optical_center = width * (-left / denom);
			float offset = optical_center - (width * 0.5f);

			if (hud_depth <= 0.0f)
			{
				hud_depth = 0.5f;
			}
			if (hud_ipd <= 0.0f)
			{
				hud_ipd = 0.064f;
			}
			if (depthScale <= 0.0f)
			{
				depthScale = 1.0f;
			}

			depth_offset = ((hud_ipd * 0.5f) / (hud_depth * depthScale)) * (width / denom);
			offset += (eye == 0) ? depth_offset : -depth_offset;

			return (int)(offset + (offset >= 0.0f ? 0.5f : -0.5f));
		}

		return 0;
	}

	return (separation > 0) ? -25 : 25;
}

static int
SCR_GetStereoHudOffset(float separation)
{
	return SCR_GetStereoHudOffsetScaled(separation, 1.0f);
}

void
DrawNumberCenteredImageScaled(int x, int y, char* num, float scale)
{
    int len = strlen(num);
    int width = 8; // half of img width
    int height = 12; // half of img height
    for(int i = 0; i < len; i++){
        char image[6];
        sprintf(image, "num_%c", num[i]);
        float offset = width * ((i * 2) - (len));
        Draw_PicScaled(x + offset, y - (height * scale), image,
                       scale);
    }
}

void
SCR_DrawItemWheel (float separation)
{
    int totalIcons;
    wheel_icon_t* iconlist;
    if(!isItems) {
        totalIcons = 11;
        iconlist = weaponIcons;
    } else {
        totalIcons = 6;
        iconlist = itemIcons;
    }
    if(draw_item_wheel) {
        int offset_stereo = SCR_GetStereoHudOffset(separation);
        int ringw, ringh;
        int curw, curh;
        int vidwc = (viddef.width/2);
        int vidhc = (viddef.height/2);
        Draw_GetPicSize(&ringw, &ringh,"/wheel/ring.png");
        Draw_PicScaled((vidwc - (ringw/2)) + offset_stereo, (vidhc - (ringh/2)), "/wheel/ring.png", 1.0f);
        Draw_GetPicSize(&curw, &curh,"/wheel/cursor.png");
        Draw_PicScaled((vidwc - (curw/2)) + ((polarCursor[0] * cosf(polarCursor[1])) * cursorFactor) + offset_stereo,
                       (vidwc - (curh/2)) + ((polarCursor[0] * sinf(polarCursor[1])) * cursorFactor),
                       "/wheel/cursor.png", 1.0f);

        for(int i = 0; i < totalIcons; i++)
        {
            if(cl.inventory[iconlist[i].index])
            { // if weapon is available in inventory
                char iconName[40];
                char ammoName[30];
                char ammoAmount[4];
                float iconFactor;
                float ammoFactor = 4.0f;
                int iconWidth = 12; // actually half of icon size. For centering purposes
                if (i == segment && polarCursor[0] > 8)
                { // if cursor is inside the segment corresponding to the item
                    // Highlighted weapon: drawn a little larger and popped slightly towards
                    // the user. The pop is done by scaling only the depth-parallax term of
                    // the stereo offset (depthScale < 1 == nearer); scaling the whole offset
                    // - as a previous *1.3f did - corrupts the FOV-centering term and makes
                    // the icon stereo-incorrect.
                    iconFactor = 3.0f;
                    int offset_stereo_selected = SCR_GetStereoHudOffsetScaled(separation, 0.9f);
                    DrawStringScaled(vidwc + offset_stereo - (strlen(iconlist[i].command) * 4),
                                     vidhc - 100,
                                     iconlist[i].command, 1.0f); // Item name
                    sprintf(iconName, "/wheel/%s_selected.png", iconlist[i].name); // selected icon path
                    Draw_PicScaled(vidwc + iconlist[i].x - (iconWidth * iconFactor) + offset_stereo_selected,
                                   vidhc + iconlist[i].y - (iconWidth * iconFactor), iconName,
                                   iconFactor);
                    if(iconlist[i].ammo) {
                        sprintf(ammoAmount, "%i", cl.inventory[iconlist[i].ammo_i]);
                        DrawNumberCenteredImageScaled(vidwc + offset_stereo, vidhc + 100,
                                                      ammoAmount,
                                                      1.0f); // ammo amount in image numbers
                        sprintf(ammoName, "/wheel/a_%s.png", iconlist[i].ammo); // ammo icon path
                        Draw_PicScaled(vidwc - (iconWidth * ammoFactor) +
                                       offset_stereo, // ammo icon for the weapon
                                       vidhc - (iconWidth * ammoFactor), ammoName,
                                       ammoFactor);
                    }
                }
                else
                {
                    iconFactor = 1.5f;
                    sprintf(iconName, "/wheel/%s.png", iconlist[i].name);
                    Draw_PicScaled(vidwc + iconlist[i].x - (iconWidth * iconFactor) + (offset_stereo),
                                   vidhc + iconlist[i].y - (iconWidth * iconFactor), iconName,
                                   iconFactor);
                }
            }
        }

    }
}

void
SCR_DrawLoading(void)
{
	int w, h;
	float scale = SCR_GetMenuScale();

	if (!scr_draw_loading)
	{
		return;
	}

	Draw_GetPicSize(&w, &h, "loading");
	Draw_PicScaled((viddef.width - w * scale) / 2, (viddef.height - h * scale) / 2, "loading", scale);
}

/*
 * Scroll it up or down
 */
void
SCR_RunConsole(void)
{
	/* decide on the height of the console */
	if (cls.key_dest == key_console)
	{
		scr_conlines = 0.5; /* half screen */
	}
	else
	{
		scr_conlines = 0; /* none visible */
	}

	if (scr_conlines < scr_con_current)
	{
		scr_con_current -= scr_conspeed->value * cls.rframetime;

		if (scr_conlines > scr_con_current)
		{
			scr_con_current = scr_conlines;
		}
	}
	else if (scr_conlines > scr_con_current)
	{
		scr_con_current += scr_conspeed->value * cls.rframetime;

		if (scr_conlines < scr_con_current)
		{
			scr_con_current = scr_conlines;
		}
	}
}

void
SCR_DrawConsole(float separation)
{
	Con_CheckResize();

	if ((cls.state == ca_disconnected) || (cls.state == ca_connecting))
	{
		/* forced full screen console */
#ifdef __ANDROID__
		Con_DrawConsole(0.5); // Always half screen for keyboard
#else
		Con_DrawConsole(1.0);
#endif
		return;
	}

	if ((cls.state != ca_active) || !cl.refresh_prepped)
	{
		/* connected, but can't render */
		Con_DrawConsole(0.5);
		Draw_Fill(0, viddef.height / 2, viddef.width, viddef.height / 2, 0);
		return;
	}

	if (scr_con_current)
	{
		Con_DrawConsole(scr_con_current);
	}
	else
	{
		if ((cls.key_dest == key_game) || (cls.key_dest == key_message))
		{
			Con_DrawNotify(); /* only draw notify in game */
		}
	}
}

void
SCR_BeginLoadingPlaque(void)
{
	S_StopAllSounds();
	cl.sound_prepped = false; /* don't play ambients */

	OGG_Stop();

	if (cls.disable_screen)
	{
		return;
	}

	if (developer->value)
	{
		return;
	}

	if (cls.state == ca_disconnected)
	{
		/* if at console, don't bring up the plaque */
		return;
	}

	if (cls.key_dest == key_console)
	{
		return;
	}

	if (cl.cinematictime > 0)
	{
		scr_draw_loading = 2; /* clear to balack first */
	}
	else
	{
		scr_draw_loading = 1;
	}

	SCR_UpdateForEye(0);

	scr_draw_loading = false;

	SCR_StopCinematic();
	cls.disable_screen = Sys_Milliseconds();
	cls.disable_servercount = cl.servercount;
}

void
SCR_EndLoadingPlaque(void)
{
	cls.disable_screen = 0;
	Con_ClearNotify();
}

void
SCR_Loading_f(void)
{
	SCR_BeginLoadingPlaque();
}

void
SCR_TimeRefresh_f(void)
{
	int i;
	int start, stop;
	float time;

	if (cls.state != ca_active)
	{
		return;
	}

	start = Sys_Milliseconds();

	if (Cmd_Argc() == 2)
	{
		/* run without page flipping */
		int j;

		for (j = 0; j < 1000; j++)
		{
			R_BeginFrame(0);

			for (i = 0; i < 128; i++)
			{
				cl.refdef.viewangles[1] = i / 128.0f * 360.0f;
				R_RenderFrame(&cl.refdef);
			}

			R_EndFrame();
		}
	}
	else
	{
		for (i = 0; i < 128; i++)
		{
			cl.refdef.viewangles[1] = i / 128.0f * 360.0f;

			R_BeginFrame(0);
			R_RenderFrame(&cl.refdef);
			R_EndFrame();
		}
	}

	stop = Sys_Milliseconds();
	time = (stop - start) / 1000.0f;
	Com_Printf("%f seconds (%f fps)\n", time, 128 / time);
}

void
SCR_AddDirtyPoint(int x, int y)
{
	if (x < scr_dirty.x1)
	{
		scr_dirty.x1 = x;
	}

	if (x > scr_dirty.x2)
	{
		scr_dirty.x2 = x;
	}

	if (y < scr_dirty.y1)
	{
		scr_dirty.y1 = y;
	}

	if (y > scr_dirty.y2)
	{
		scr_dirty.y2 = y;
	}
}

void
SCR_DirtyScreen(void)
{
	SCR_AddDirtyPoint(0, 0);
	SCR_AddDirtyPoint(viddef.width - 1, viddef.height - 1);
}

/*
 * Clear any parts of the tiled background that were drawn on last frame
 */
void
SCR_TileClear(void)
{
	int i;
	int top, bottom, left, right;
	dirty_t clear;

	if (scr_con_current == 1.0)
	{
		return; /* full screen console */
	}

	if (scr_viewsize->value == 100)
	{
		return; /* full screen rendering */
	}

	if (cl.cinematictime > 0)
	{
		return; /* full screen cinematic */
	}

	/* erase rect will be the union of the past three
	   frames so tripple buffering works properly */
	clear = scr_dirty;

	for (i = 0; i < 2; i++)
	{
		if (scr_old_dirty[i].x1 < clear.x1)
		{
			clear.x1 = scr_old_dirty[i].x1;
		}

		if (scr_old_dirty[i].x2 > clear.x2)
		{
			clear.x2 = scr_old_dirty[i].x2;
		}

		if (scr_old_dirty[i].y1 < clear.y1)
		{
			clear.y1 = scr_old_dirty[i].y1;
		}

		if (scr_old_dirty[i].y2 > clear.y2)
		{
			clear.y2 = scr_old_dirty[i].y2;
		}
	}

	scr_old_dirty[1] = scr_old_dirty[0];
	scr_old_dirty[0] = scr_dirty;

	scr_dirty.x1 = 9999;
	scr_dirty.x2 = -9999;
	scr_dirty.y1 = 9999;
	scr_dirty.y2 = -9999;

	/* don't bother with anything convered by the console */
	top = (int)(scr_con_current * viddef.height);

	if (top >= clear.y1)
	{
		clear.y1 = top;
	}

	if (clear.y2 <= clear.y1)
	{
		return; /* nothing disturbed */
	}

	top = scr_vrect.y;
	bottom = top + scr_vrect.height - 1;
	left = scr_vrect.x;
	right = left + scr_vrect.width - 1;

	if (clear.y1 < top)
	{
		/* clear above view screen */
		i = clear.y2 < top - 1 ? clear.y2 : top - 1;
		Draw_TileClear(clear.x1, clear.y1,
				clear.x2 - clear.x1 + 1, i - clear.y1 + 1, "backtile");
		clear.y1 = top;
	}

	if (clear.y2 > bottom)
	{
		/* clear below view screen */
		i = clear.y1 > bottom + 1 ? clear.y1 : bottom + 1;
		Draw_TileClear(clear.x1, i,
				clear.x2 - clear.x1 + 1, clear.y2 - i + 1, "backtile");
		clear.y2 = bottom;
	}

	if (clear.x1 < left)
	{
		/* clear left of view screen */
		i = clear.x2 < left - 1 ? clear.x2 : left - 1;
		Draw_TileClear(clear.x1, clear.y1,
				i - clear.x1 + 1, clear.y2 - clear.y1 + 1, "backtile");
		clear.x1 = left;
	}

	if (clear.x2 > right)
	{
		/* clear left of view screen */
		i = clear.x1 > right + 1 ? clear.x1 : right + 1;
		Draw_TileClear(i, clear.y1,
				clear.x2 - i + 1, clear.y2 - clear.y1 + 1, "backtile");
		clear.x2 = right;
	}
}

#define STAT_MINUS 10
char *sb_nums[2][11] = {
	{
		"num_0", "num_1", "num_2", "num_3", "num_4", "num_5",
		"num_6", "num_7", "num_8", "num_9", "num_minus"
	},
	{
		"anum_0", "anum_1", "anum_2", "anum_3", "anum_4", "anum_5",
		"anum_6", "anum_7", "anum_8", "anum_9", "anum_minus"
	}
};

#define ICON_WIDTH 24
#define ICON_HEIGHT 24
#define CHAR_WIDTH 16
#define ICON_SPACE 8

/*
 * Allow embedded \n in the string
 */
void
SizeHUDString(char *string, int *w, int *h)
{
	int lines, width, current;

	lines = 1;
	width = 0;

	current = 0;

	while (*string)
	{
		if (*string == '\n')
		{
			lines++;
			current = 0;
		}
		else
		{
			current++;

			if (current > width)
			{
				width = current;
			}
		}

		string++;
	}

	*w = width * 8;
	*h = lines * 8;
}

void
DrawHUDStringScaled(char *string, int x, int y, int centerwidth, int xor, float factor)
{
	int margin;
	char line[1024];
	int width;
	int i;

	margin = x;

	while (*string)
	{
		/* scan out one line of text from the string */
		width = 0;

		while (*string && *string != '\n')
		{
			line[width++] = *string++;
		}

		line[width] = 0;

		if (centerwidth)
		{
			x = margin + (centerwidth - width * 8)*factor / 2;
		}

		else
		{
			x = margin;
		}

		for (i = 0; i < width; i++)
		{
			Draw_CharScaled(x, y, line[i] ^ xor, factor);
			x += 8*factor;
		}

		if (*string)
		{
			string++; /* skip the \n */
			y += 8*factor;
		}
	}
}

void
DrawHUDString(char *string, int x, int y, int centerwidth, int xor)
{
	DrawHUDStringScaled(string, x, y, centerwidth, xor, 1.0f);
}

void
SCR_DrawFieldScaled(int x, int y, int color, int width, int value, float factor)
{
	char num[16], *ptr;
	int l;
	int frame;

	if (width < 1)
	{
		return;
	}

	/* draw number string */
	if (width > 5)
	{
		width = 5;
	}

	SCR_AddDirtyPoint(x, y);
	SCR_AddDirtyPoint(x + (width * CHAR_WIDTH + 2)*factor, y + factor*24);

	Com_sprintf(num, sizeof(num), "%i", value);
	l = (int)strlen(num);

	if (l > width)
	{
		l = width;
	}

	x += (2 + CHAR_WIDTH * (width - l)) * factor;

	ptr = num;

	while (*ptr && l)
	{
		if (*ptr == '-')
		{
			frame = STAT_MINUS;
		}

		else
		{
			frame = *ptr - '0';
		}

		Draw_PicScaled(x, y, sb_nums[color][frame], factor);
		x += CHAR_WIDTH*factor;
		ptr++;
		l--;
	}
}

void
SCR_DrawField(int x, int y, int color, int width, int value)
{
	SCR_DrawFieldScaled(x, y, color, width, value, 1.0f);
}

/*
 * Allows rendering code to cache all needed sbar graphics
 */
void
SCR_TouchPics(void)
{
	int i, j;

	for (i = 0; i < 2; i++)
	{
		for (j = 0; j < 11; j++)
		{
			Draw_FindPic(sb_nums[i][j]);
		}
	}

	if (crosshair->value)
	{
		if ((crosshair->value > 3) || (crosshair->value < 0))
		{
			crosshair->value = 3;
		}

		Com_sprintf(crosshair_pic, sizeof(crosshair_pic), "ch%i",
				(int)(crosshair->value));
		Draw_GetPicSize(&crosshair_width, &crosshair_height, crosshair_pic);

		if (!crosshair_width)
		{
			crosshair_pic[0] = 0;
		}
	}
}

void
SCR_ExecuteLayoutString(char *s,float separation)
{
	int x, y;
	int value;
	char *token;
	int width;
	int index;
	clientinfo_t *ci;

	float scale = SCR_GetHUDScale();

	if ((cls.state != ca_active) || !cl.refresh_prepped)
	{
		return;
	}

	if (!s[0])
	{
		return;
	}

	x = 0;
	y = 0;

	int offset_stereo = SCR_GetStereoHudOffset(separation);

	while (s)
	{
		token = COM_Parse(&s);

		if (!strcmp(token, "xl"))
		{
			token = COM_Parse(&s);
			x = scale*(int)strtol(token, (char **)NULL, 10) + offset_stereo;
			continue;
		}

		if (!strcmp(token, "xr"))
		{
			token = COM_Parse(&s);
			x = viddef.width + scale*(int)strtol(token, (char **)NULL, 10) + offset_stereo;
			continue;
		}

		if (!strcmp(token, "xv"))
		{
			token = COM_Parse(&s);
			x = viddef.width / 2 - scale*160 + scale*(int)strtol(token, (char **)NULL, 10) + offset_stereo;
			continue;
		}
		if (!strcmp(token, "xh"))
		{
			token = COM_Parse (&s);
			x = viddef.width/2 - 160 + atoi(token) + offset_stereo;
			continue;
		}

		if (!strcmp(token, "yt"))
		{
			token = COM_Parse(&s);
			y = viddef.height/3 + scale*(int)strtol(token, (char **)NULL, 10);
			continue;
		}

		if (!strcmp(token, "yb"))
		{
			token = COM_Parse(&s);
			y = (viddef.height * 0.72) + scale*(int)strtol(token, (char **)NULL, 10);
			continue;
		}

		if (!strcmp(token, "yv"))
		{
			token = COM_Parse(&s);
			y = viddef.height / 2 + scale*(int)strtol(token, (char **)NULL, 10);
			continue;
		}

		if (!strcmp(token, "pic"))
		{
			/* draw a pic from a stat number */
			token = COM_Parse(&s);
			index = (int)strtol(token, (char **)NULL, 10);

			if ((index < 0) || (index >= sizeof(cl.frame.playerstate.stats)))
			{
				Com_Error(ERR_DROP, "bad stats index %d (0x%x)", index, index);
			}

			value = cl.frame.playerstate.stats[index];

			if (value >= MAX_IMAGES)
			{
				Com_Error(ERR_DROP, "Pic >= MAX_IMAGES");
			}

			if (cl.configstrings[CS_IMAGES + value])
			{
				SCR_AddDirtyPoint(x, y);
				SCR_AddDirtyPoint(x + 23*scale, y + 23*scale);
				Draw_PicScaled(x, y, cl.configstrings[CS_IMAGES + value], scale);
			}

			continue;
		}

		if (!strcmp(token, "client"))
		{
			/* draw a deathmatch client block */
			int score, ping, time;

			token = COM_Parse(&s);
			x = viddef.width / 2 - scale*160 + scale*(int)strtol(token, (char **)NULL, 10);
			token = COM_Parse(&s);
			y = viddef.height / 2 + scale*(int)strtol(token, (char **)NULL, 10);
			SCR_AddDirtyPoint(x, y);
			SCR_AddDirtyPoint(x + scale*159, y + scale*31);

			token = COM_Parse(&s);
			value = (int)strtol(token, (char **)NULL, 10);

			if ((value >= MAX_CLIENTS) || (value < 0))
			{
				Com_Error(ERR_DROP, "client >= MAX_CLIENTS");
			}

			ci = &cl.clientinfo[value];

			token = COM_Parse(&s);
			score = (int)strtol(token, (char **)NULL, 10);

			token = COM_Parse(&s);
			ping = (int)strtol(token, (char **)NULL, 10);

			token = COM_Parse(&s);
			time = (int)strtol(token, (char **)NULL, 10);

			DrawAltStringScaled(x + scale*32, y, ci->name, scale);
			DrawAltStringScaled(x + scale*32, y + scale*8, "Score: ", scale);
			DrawAltStringScaled(x + scale*(32 + 7 * 8), y + scale*8, va("%i", score), scale);
			DrawStringScaled(x + scale*32, y + scale*16, va("Ping:  %i", ping), scale);
			DrawStringScaled(x + scale*32, y + scale*24, va("Time:  %i", time), scale);

			if (!ci->icon)
			{
				ci = &cl.baseclientinfo;
			}

			Draw_PicScaled(x, y, ci->iconname, scale);
			continue;
		}

		if (!strcmp(token, "ctf"))
		{
			/* draw a ctf client block */
			int score, ping;
			char block[80];

			token = COM_Parse(&s);
			x = viddef.width / 2 - scale*160 + scale*(int)strtol(token, (char **)NULL, 10);
			token = COM_Parse(&s);
			y = viddef.height / 2 + scale*(int)strtol(token, (char **)NULL, 10);
			SCR_AddDirtyPoint(x, y);
			SCR_AddDirtyPoint(x + scale*159, y + scale*31);

			token = COM_Parse(&s);
			value = (int)strtol(token, (char **)NULL, 10);

			if ((value >= MAX_CLIENTS) || (value < 0))
			{
				Com_Error(ERR_DROP, "client >= MAX_CLIENTS");
			}

			ci = &cl.clientinfo[value];

			token = COM_Parse(&s);
			score = (int)strtol(token, (char **)NULL, 10);

			token = COM_Parse(&s);
			ping = (int)strtol(token, (char **)NULL, 10);

			if (ping > 999)
			{
				ping = 999;
			}

			sprintf(block, "%3d %3d %-12.12s", score, ping, ci->name);

			if (value == cl.playernum)
			{
				DrawAltStringScaled(x, y, block, scale);
			}

			else
			{
				DrawStringScaled(x, y, block, scale);
			}

			continue;
		}

		if (!strcmp(token, "picn"))
		{
			/* draw a pic from a name */
			token = COM_Parse(&s);
			SCR_AddDirtyPoint(x, y);
			SCR_AddDirtyPoint(x + scale*23, y + scale*23);
			Draw_PicScaled(x, y, (char *)token, scale);
			continue;
		}

		if (!strcmp(token, "num"))
		{
			/* draw a number */
			token = COM_Parse(&s);
			width = (int)strtol(token, (char **)NULL, 10);
			token = COM_Parse(&s);
			value = cl.frame.playerstate.stats[(int)strtol(token, (char **)NULL, 10)];
			SCR_DrawFieldScaled(x, y, 0, width, value, scale);
			continue;
		}

		if (!strcmp(token, "hnum"))
		{
			/* health number */
			int color;

			width = 3;
			value = cl.frame.playerstate.stats[STAT_HEALTH];

			if (value > 25)
			{
				color = 0;  /* green */
			}
			else if (value > 0)
			{
				color = (cl.frame.serverframe >> 2) & 1; /* flash */
			}
			else
			{
				color = 1;
			}

			if (cl.frame.playerstate.stats[STAT_FLASHES] & 1)
			{
				Draw_PicScaled(x, y, "field_3", scale);
			}

			SCR_DrawFieldScaled(x, y, color, width, value, scale);
			continue;
		}

		if (!strcmp(token, "anum"))
		{
			/* ammo number */
			int color;

			width = 3;
			value = cl.frame.playerstate.stats[STAT_AMMO];

			if (value > 5)
			{
				color = 0; /* green */
			}
			else if (value >= 0)
			{
				color = (cl.frame.serverframe >> 2) & 1; /* flash */
			}
			else
			{
				continue; /* negative number = don't show */
			}

			if (cl.frame.playerstate.stats[STAT_FLASHES] & 4)
			{
				Draw_PicScaled(x, y, "field_3", scale);
			}

			SCR_DrawFieldScaled(x, y, color, width, value, scale);
			continue;
		}

		if (!strcmp(token, "rnum"))
		{
			/* armor number */
			int color;

			width = 3;
			value = cl.frame.playerstate.stats[STAT_ARMOR];

			if (value < 1)
			{
				continue;
			}

			color = 0; /* green */

			if (cl.frame.playerstate.stats[STAT_FLASHES] & 2)
			{
				Draw_PicScaled(x, y, "field_3", scale);
			}

			SCR_DrawFieldScaled(x, y, color, width, value, scale);
			continue;
		}

		if (!strcmp(token, "stat_string"))
		{
			token = COM_Parse(&s);
			index = (int)strtol(token, (char **)NULL, 10);

			if ((index < 0) || (index >= MAX_CONFIGSTRINGS))
			{
				Com_Error(ERR_DROP, "Bad stat_string index");
			}

			index = cl.frame.playerstate.stats[index];

			if ((index < 0) || (index >= MAX_CONFIGSTRINGS))
			{
				Com_Error(ERR_DROP, "Bad stat_string index");
			}

			DrawStringScaled(x, y, cl.configstrings[index], scale);
			continue;
		}

		if (!strcmp(token, "cstring"))
		{
			token = COM_Parse(&s);
			DrawHUDStringScaled(token, x, y, 320, 0, scale); // FIXME: or scale 320 here?
			continue;
		}

		if (!strcmp(token, "string"))
		{
			token = COM_Parse(&s);
			DrawStringScaled(x, y, token, scale);
			continue;
		}

		if (!strcmp(token, "cstring2"))
		{
			token = COM_Parse(&s);
			DrawHUDStringScaled(token, x, y, 320, 0x80, scale); // FIXME: or scale 320 here?
			continue;
		}

		if (!strcmp(token, "string2"))
		{
			token = COM_Parse(&s);
			DrawAltStringScaled(x, y, token, scale);
			continue;
		}

		if (!strcmp(token, "if"))
		{
			/* draw a number */
			token = COM_Parse(&s);
			value = cl.frame.playerstate.stats[(int)strtol(token, (char **)NULL, 10)];

			if (!value)
			{
				/* skip to endif */
				while (s && strcmp(token, "endif"))
				{
					token = COM_Parse(&s);
				}
			}

			continue;
		}
	}
}

/*
 * The status bar is a small layout program that
 * is based on the stats array
 */
void
SCR_DrawStats(float separation)
{
	SCR_ExecuteLayoutString(cl.configstrings[CS_STATUSBAR], separation);
}

#define STAT_LAYOUTS 13

void
SCR_DrawLayout(float separation)
{
	if (!cl.frame.playerstate.stats[STAT_LAYOUTS])
	{
		return;
	}

	SCR_ExecuteLayoutString(cl.layout, separation);
}

// ----

void
SCR_Framecounter(void) {
	long long newtime;
	static int frame;
	static int frametimes[60] = {0};
	static long long oldtime;

	newtime = Sys_Microseconds();
	frametimes[frame] = (int)(newtime - oldtime);

	oldtime = newtime;
	frame++;
	if (frame > 59) {
		frame = 0;
	}

	float scale = SCR_GetConsoleScale();

	if (cl_showfps->value == 1) {
		// Calculate average of frames.
		int avg = 0;
		int num = 0;

		for (int i = 0; i < 60; i++) {
			if (frametimes[i] != 0) {
				avg += frametimes[i];
				num++;
			}
		}


		char str[10];
		snprintf(str, sizeof(str), "%3.2ffps", (1000.0 * 1000.0) / (avg / num));
		DrawStringScaled(viddef.width - scale*(strlen(str)*8 + 2), 0, str, scale);
	} else if (cl_showfps->value >= 2) {
		// Calculate average of frames.
		int avg = 0;
		int num = 0;

		for (int i = 0; i < 60; i++) {
			if (frametimes[i] != 0) {
				avg += frametimes[i];
				num++;
			}
		}


		// Find lowest and highest
		int min = frametimes[0];
		int max = frametimes[1];

		for (int i = 1; i < 60; i++) {
			if ((frametimes[i] > 0) &&  (min < frametimes[i])) {
				min = frametimes[i];
			}

			if ((frametimes[i] > 0) && (max > frametimes[i])) {
				max = frametimes[i];
			}
		}

		char str[64];
		snprintf(str, sizeof(str), "Min: %7.2ffps, Max: %7.2ffps, Avg: %7.2ffps",
		         (1000.0 * 1000.0) / min, (1000.0 * 1000.0) / max, (1000.0 * 1000.0) / (avg / num));
		DrawStringScaled(viddef.width - scale*(strlen(str)*8 + 2), 0, str, scale);

		if (cl_showfps->value > 2)
		{
			snprintf(str, sizeof(str), "Max: %5.2fms, Min: %5.2fms, Avg: %5.2fms",
			         0.001f*min, 0.001f*max, 0.001f*(avg / num));
			DrawStringScaled(viddef.width - scale*(strlen(str)*8 + 2), scale*10, str, scale);
		}
	}
}

// ----
/*
 * This is called every frame, and can also be called
 * explicitly to flush text to the screen.
 */
void SCR_UpdateForEye (int eye)
{
	int numframes = 1;
	int i;
	float separation = 0;
	float scale = SCR_GetMenuScale();

	/* if the screen is disabled (loading plaque is
	   up, or vid mode changing) do nothing at all */
	if (cls.disable_screen)
	{
		if (Sys_Milliseconds() - cls.disable_screen > 120000)
		{
			cls.disable_screen = 0;
			Com_Printf("Loading plaque timed out.\n");
		}

		return;
	}

	if (!scr_initialized || !con.initialized)
	{
		return; /* not initialized yet */
	}

	//World scale based separation
	separation = ((-vr_worldscale->value * 0.065f) / 2) * (1 - (2*eye));

	for (i = 0; i < numframes; i++)
	{
		R_BeginFrame(separation);

		if (scr_draw_loading == 2)
		{
			/* loading plaque over black screen */
			int w, h;

			if(i == 0){
				R_SetPalette(NULL);
			}

			if(i == numframes - 1){
				scr_draw_loading = false;
			}

			Draw_GetPicSize(&w, &h, "loading");
			Draw_PicScaled((viddef.width - w * scale) / 2, (viddef.height - h * scale) / 2, "loading", scale);
		}

		/* if a cinematic is supposed to be running,
		   handle menus and console specially */
		else if (cl.cinematictime > 0)
		{
			if (cls.key_dest == key_menu)
			{
				if (cl.cinematicpalette_active)
				{
					R_SetPalette(NULL);
					cl.cinematicpalette_active = false;
				}

				M_Draw();
			}
			else if (cls.key_dest == key_console)
			{
				if (cl.cinematicpalette_active)
				{
					R_SetPalette(NULL);
					cl.cinematicpalette_active = false;
				}

				SCR_DrawConsole(separation);
			}
			else
			{
				SCR_DrawCinematic();
			}
		}
		else
		{
			/* make sure the game palette is active */
			if (cl.cinematicpalette_active)
			{
				R_SetPalette(NULL);
				cl.cinematicpalette_active = false;
			}

			/* do 3D refresh drawing, and then update the screen */
			SCR_CalcVrect();

			/* clear any dirty part of the background */
			SCR_TileClear();

			V_RenderView(separation);

            SCR_DrawVignette(separation);

			SCR_DrawStats(separation);

			if (cl.frame.playerstate.stats[STAT_LAYOUTS] & 1)
			{
				SCR_DrawLayout(separation);
			}

			if (cl.frame.playerstate.stats[STAT_LAYOUTS] & 2)
			{
				CL_DrawInventory(separation);
			}

            SCR_DrawItemWheel(separation);

			SCR_DrawNet(separation);
			SCR_CheckDrawCenterString(separation);

			if (scr_timegraph->value)
			{
				SCR_DebugGraph(cls.rframetime * 300, 0);
			}

			if (scr_debuggraph->value || scr_timegraph->value ||
				scr_netgraph->value)
			{
				SCR_DrawDebugGraph();
			}

			SCR_DrawPause(separation);

			SCR_DrawConsole(separation);

			M_Draw();

			SCR_DrawLoading();
		}
	}

	R_EndFrame();
}

static float
SCR_ClampScale(float scale)
{
	float f;

	f = viddef.width / 320.0f;
	if (scale > f)
	{
		scale = f;
	}

	f = viddef.height / 240.0f;
	if (scale > f)
	{
		scale = f;
	}

	if (scale < 1)
	{
		scale = 1;
	}

	return scale;
}

static float
SCR_GetDefaultScale(void)
{
/*	int i = viddef.width / 640;
	int j = viddef.height / 240;

	if (i > j)
	{
		i = j;
	}
	if (i < 1)
	{
		i = 1;
	}
	return i;
*/

    return 1;
}

void
SCR_DrawCrosshair(void)
{
	float scale;

	if (!crosshair->value)
	{
		return;
	}

	if (crosshair->modified)
	{
		crosshair->modified = false;
		SCR_TouchPics();
	}

	if (!crosshair_pic[0])
	{
		return;
	}

	if (crosshair_scale->value < 0)
	{
		scale = SCR_GetDefaultScale();
	}
	else
	{
		scale = SCR_ClampScale(crosshair_scale->value);
	}

	Draw_PicScaled(scr_vrect.x + (scr_vrect.width - crosshair_width * scale) / 2,
			scr_vrect.y + (scr_vrect.height - crosshair_height * scale) / 2,
			crosshair_pic, scale);
}

float
SCR_GetHUDScale(void)
{
	float scale;

	if (!scr_initialized)
	{
		scale = 1;
	}
	else if (r_hudscale->value < 0)
	{
		scale = SCR_GetDefaultScale();
	}
	else if (r_hudscale->value == 0) /* HACK: allow scale 0 to hide the HUD */
	{
		scale = 0;
	}
	else
	{
		scale = SCR_ClampScale(r_hudscale->value);
	}

	return scale;
}

float
SCR_GetConsoleScale(void)
{
	float scale;

	if (!scr_initialized)
	{
		scale = 1;
	}
	else if (r_consolescale->value < 0)
	{
		scale = SCR_GetDefaultScale();
	}
	else
	{
		scale = SCR_ClampScale(r_consolescale->value);
	}

	return scale;
}

float
SCR_GetMenuScale(void)
{
	float scale;

	if (!scr_initialized)
	{
		scale = 1;
	}
	else if (r_menuscale->value < 0)
	{
		scale = SCR_GetDefaultScale();
	}
	else
	{
		scale = SCR_ClampScale(r_menuscale->value);
	}

	return scale;
}
