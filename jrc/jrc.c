/*
 *  Hamlib JRC backend - main file
 *  Copyright (c) 2001-2004 by Stephane Fillod
 *
 *	$Id: jrc.c,v 1.12 2004-05-19 08:57:45 fillods Exp $
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Library General Public License as
 *   published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Library General Public License for more details.
 *
 *   You should have received a copy of the GNU Library General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>  /* String function definitions */
#include <unistd.h>  /* UNIX standard function definitions */
#include <math.h>

#include "hamlib/rig.h"
#include "serial.h"
#include "misc.h"
#include "cal.h"
#include "register.h"

#include "jrc.h"


/*
 * Carriage return
 */
#define EOM "\r"

#define BUFSZ 32

/*
 * modes in use by the "2G" command
 */
#define MD_RTTY	'0'
#define MD_CW	'1'
#define MD_USB	'2'
#define MD_LSB	'3'
#define MD_AM	'4'
#define MD_FM 	'5'
#define MD_AMS	'6'
#define MD_ECCS_USB	'7'
#define MD_ECCS_LSB	'8'
#define MD_WFM	'9'


/*
 * jrc_transaction
 * We assume that rig!=NULL, rig->state!= NULL, data!=NULL, data_len!=NULL
 * Otherwise, you'll get a nice seg fault. You've been warned!
 * TODO: error case handling
 */
int jrc_transaction(RIG *rig, const char *cmd, int cmd_len, char *data, int *data_len)
{
	int retval;
	struct rig_state *rs;

	rs = &rig->state;

	serial_flush(&rs->rigport);

	retval = write_block(&rs->rigport, cmd, cmd_len);
	if (retval != RIG_OK)
			return retval;

	/* no data expected, TODO: flush input? */
	if (!data || !data_len)
			return 0;

	*data_len = read_string(&rs->rigport, data, BUFSZ, EOM, strlen(EOM));

	return RIG_OK;
}


int jrc_open(RIG *rig)
{
  int retval;

  /* Turning computer control ON */

  retval = jrc_transaction(rig, "H1" EOM, 3, NULL, NULL);
  return retval;
}

int jrc_close(RIG *rig)
{
  int retval;

  /* Turning computer control OFF */

  retval = jrc_transaction(rig, "H0" EOM, 3, NULL, NULL);
  return retval;
}


/*
 * jrc_set_freq
 * Assumes rig!=NULL
 */
int jrc_set_freq(RIG *rig, vfo_t vfo, freq_t freq)
{
		struct jrc_priv_caps *priv = (struct jrc_priv_caps*)rig->caps->priv;
		char freqbuf[BUFSZ];
		int freq_len;

		if (freq >= (freq_t)pow(10, priv->max_freq_len))
		  return -RIG_EINVAL;

		freq_len = sprintf(freqbuf, "F%0*Ld" EOM, priv->max_freq_len, (long long)freq);

		return jrc_transaction (rig, freqbuf, freq_len, NULL, NULL);
}

/*
 * jrc_get_freq
 * Assumes rig!=NULL, freq!=NULL
 */
int jrc_get_freq(RIG *rig, vfo_t vfo, freq_t *freq)
{
		struct jrc_priv_caps *priv = (struct jrc_priv_caps*)rig->caps->priv;
		int freq_len, retval;
		char freqbuf[BUFSZ];
		long long f;

		//note: JRCs use "I" to get information
		retval = jrc_transaction (rig, "I" EOM, 2, freqbuf, &freq_len);
		if (retval != RIG_OK)
		  return retval;
				
		//I command returns Iabdffffffffg<CR>
		if (freqbuf[0] != 'I' || freq_len != priv->info_len) {
		  rig_debug(RIG_DEBUG_ERR,"jrc_get_freq: wrong answer %s, "
			    "len=%d\n", freqbuf, freq_len);
		  return -RIG_ERJCTED;
		}
		freqbuf[4+priv->max_freq_len] = '\0';

		/* extract freq */
		sscanf(freqbuf+4, "%llu", &f);
		*freq = f;

		return RIG_OK;
}

/*
 * jrc_set_mode
 * Assumes rig!=NULL
 */
int jrc_set_mode(RIG *rig, vfo_t vfo, rmode_t mode, pbwidth_t width)
{
		char mdbuf[BUFSZ];
		int retval, mdbuf_len;
		char amode;
		const char *bandwidth;

		switch (mode) {
		  /* FIXME: ECSS modes */
		case RIG_MODE_CW:       amode = MD_CW; break;
		case RIG_MODE_USB:      amode = MD_USB; break;
		case RIG_MODE_LSB:      amode = MD_LSB; break;
		case RIG_MODE_FM:       amode = MD_FM; break;
		case RIG_MODE_AM:       amode = MD_AM; break;
		case RIG_MODE_RTTY:	amode = MD_RTTY; break;
		case RIG_MODE_WFM:	amode = MD_WFM; break;
		case RIG_MODE_AMS:	amode = MD_AMS; break;
		default:
		  rig_debug(RIG_DEBUG_ERR,
			    "jrc_set_mode: unsupported mode %d\n", mode);
		  return -RIG_EINVAL;
		}

		mdbuf_len = sprintf(mdbuf, "D" "%c" EOM, amode);
		retval = jrc_transaction (rig, mdbuf, mdbuf_len, NULL, NULL);
		if (retval != RIG_OK)
		  return retval;

		if (width == RIG_PASSBAND_NORMAL)
			width = rig_passband_normal(rig, mode);

		if (width <= s_Hz(1500))
		  bandwidth = "B2" EOM; /*narr*/
		else if (width <= s_Hz(4000))
		  bandwidth = "B1" EOM; /*inter*/
		else if (width <= s_Hz(9000))
		  bandwidth = "B0" EOM; /*wide*/
		else if (rig->caps->rig_model == RIG_MODEL_NRD535)
		  bandwidth = "B3" EOM; /*aux - nrd535 only*/
		else
		  bandwidth = "B1" EOM; /*inter*/

		retval = jrc_transaction (rig, bandwidth, 3, NULL, NULL);

		return retval;
}

/*
 * jrc_get_mode
 * Assumes rig!=NULL, freq!=NULL
 */
int jrc_get_mode(RIG *rig, vfo_t vfo, rmode_t *mode, pbwidth_t *width)
{
		struct jrc_priv_caps *priv = (struct jrc_priv_caps*)rig->caps->priv;
		int freq_len, retval;
		char freqbuf[BUFSZ];
		char cmode;
		char cwidth;
		
		//note: JRCs use "I" to get information
		retval = jrc_transaction (rig, "I" EOM, 2, freqbuf, &freq_len);
		if (retval != RIG_OK)
		  return retval;

		//I command returns Iabdffffffffg<CR>
		if (freqbuf[0] != 'I' || freq_len != priv->info_len) {
		  rig_debug(RIG_DEBUG_ERR,"jrc_get_mode: wrong answer %s, "
			    "len=%d\n", freqbuf, freq_len);
		  return -RIG_ERJCTED;
		}

		/* extract width and mode */
		cwidth = freqbuf[2];
		cmode = freqbuf[3];

		switch(cwidth) {
		case '0' : *width = s_Hz(6000); break; //wide
		case '1' : *width = s_Hz(2000); break; //inter
		case '2' : *width = s_Hz(1000); break; //narr
		case '3' : *width = s_Hz(12000); break; //aux - nrd535 only
		default:
		  rig_debug(RIG_DEBUG_ERR,
			    "jrc_get_mode: unsupported width %c\n",
			    cwidth);
		  *width = RIG_PASSBAND_NORMAL;
		  return -RIG_EINVAL;
		}

		switch(cmode) {
		  /* FIXME: FAX/AMS, and ECSS modes */
		case '0' : *mode = RIG_MODE_RTTY; break;
		case '1' : *mode = RIG_MODE_CW; break;
		case '2' : *mode = RIG_MODE_USB; break;
		case '3' : *mode = RIG_MODE_LSB; break;
		case '4' : *mode = RIG_MODE_AM; break;
		case '5' : *mode = RIG_MODE_FM; break;
		case '6' : *mode = RIG_MODE_AMS; break;  //FAX on nrd535
		case '7' : *mode = RIG_MODE_AMS; break;  //ECSS-USB
		case '8' : *mode = RIG_MODE_AMS; break;  //ECSS-LSB
		case '9' : *mode = RIG_MODE_WFM; break;  //nrd545 only
		default:
		  rig_debug(RIG_DEBUG_ERR,
			    "jrc_get_mode: unsupported mode %c\n",
			    cmode);
		  *mode = RIG_MODE_NONE;
		  return -RIG_EINVAL;
		}

		return RIG_OK;
}

/*
 * jrc_set_func
 * Assumes rig!=NULL
 */
int jrc_set_func(RIG *rig, vfo_t vfo, setting_t func, int status)
{
		int cmd_len;
		char cmdbuf[BUFSZ];

		/* Optimize:
		 *   sort the switch cases with the most frequent first
		 */
		switch (func) {
		case RIG_FUNC_FAGC:
				/* FIXME: FAGC levels */
			cmd_len = sprintf(cmdbuf, "G%d" EOM, status?1:2);

			return jrc_transaction (rig, cmdbuf, cmd_len, NULL, NULL);

		case RIG_FUNC_NB:
			/* FIXME: NB1 and NB2 */
			cmd_len = sprintf(cmdbuf, "N%d" EOM, status?1:0);

			return jrc_transaction (rig, cmdbuf, cmd_len, NULL, NULL);

			/*
			 * FIXME: which BB mode for NR and BC at same time ?
			 */
		case RIG_FUNC_NR:
			cmd_len = sprintf(cmdbuf, "BB%d" EOM, status?1:0);

			return jrc_transaction (rig, cmdbuf, cmd_len, NULL, NULL);

		case RIG_FUNC_BC:
			cmd_len = sprintf(cmdbuf, "BB%d" EOM, status?2:0);

			return jrc_transaction (rig, cmdbuf, cmd_len, NULL, NULL);

		case RIG_FUNC_LOCK:
			cmd_len = sprintf(cmdbuf, "DD%d" EOM, status?1:0);

			return jrc_transaction (rig, cmdbuf, cmd_len, NULL, NULL);

		default:
			rig_debug(RIG_DEBUG_ERR,"Unsupported set_func %d\n", func);
			return -RIG_EINVAL;
		}

		return RIG_OK;
}


/*
 * jrc_get_func
 * Assumes rig!=NULL, status!=NULL
 */
int jrc_get_func(RIG *rig, vfo_t vfo, setting_t func, int *status)
{
		struct jrc_priv_caps *priv = (struct jrc_priv_caps*)rig->caps->priv;
		int retval, func_len;
		char funcbuf[BUFSZ];

		/* Optimize:
		 *   sort the switch cases with the most frequent first
		 */
		switch (func) {
		case RIG_FUNC_FAGC:
		  /* FIXME: FAGC levels */
		  //retval = jrc_transaction (rig, "G" EOM, 2, funcbuf, &func_len);
		  retval = jrc_transaction (rig, "I" EOM, 2, funcbuf, &func_len);
		  if (retval != RIG_OK)
		    return retval;

		  //if (func_len != 3 || func_len != 6) {
		if (funcbuf[0] != 'I' || func_len != priv->info_len) {
		    rig_debug(RIG_DEBUG_ERR,"jrc_get_func: wrong answer %s, "
			      "len=%d\n", funcbuf, func_len);
		    return -RIG_ERJCTED;
		  }
		  //*status = funcbuf[1] != '2';
		  *status = funcbuf[4+priv->max_freq_len] != '2';
		  
		  return RIG_OK;

		case RIG_FUNC_NB:
			/* FIXME: NB1 and NB2 */
			retval = jrc_transaction (rig, "N" EOM, 2, funcbuf, &func_len);
			if (retval != RIG_OK)
				return retval;

			if (func_len != 3) {
				rig_debug(RIG_DEBUG_ERR,"jrc_get_func: wrong answer %s, "
							"len=%d\n", funcbuf, func_len);
				return -RIG_ERJCTED;
			}
			*status = funcbuf[1] != '0';

			return RIG_OK;

			/*
			 * FIXME: which BB mode for NR and BC at same time ?
			 */
		case RIG_FUNC_NR:
			retval = jrc_transaction (rig, "BB" EOM, 3, funcbuf, &func_len);
			if (retval != RIG_OK)
				return retval;

			if (func_len != 3) {
				rig_debug(RIG_DEBUG_ERR,"jrc_get_func: wrong answer %s, "
							"len=%d\n", funcbuf, func_len);
				return -RIG_ERJCTED;
			}
			*status = funcbuf[2] == '1';

			return RIG_OK;

		case RIG_FUNC_BC:
			retval = jrc_transaction (rig, "BB" EOM, 3, funcbuf, &func_len);
			if (retval != RIG_OK)
				return retval;

			if (func_len != 3) {
				rig_debug(RIG_DEBUG_ERR,"jrc_get_func: wrong answer %s, "
							"len=%d\n", funcbuf, func_len);
				return -RIG_ERJCTED;
			}
			*status = funcbuf[2] == '2';

			return RIG_OK;

		case RIG_FUNC_LOCK:
			retval = jrc_transaction (rig, "DD" EOM, 3, funcbuf, &func_len);
			if (retval != RIG_OK)
				return retval;

			if (func_len != 3) {
				rig_debug(RIG_DEBUG_ERR,"jrc_get_func: wrong answer %s, "
							"len=%d\n", funcbuf, func_len);
				return -RIG_ERJCTED;
			}
			*status = funcbuf[1] == '1';

			return RIG_OK;

		default:
			rig_debug(RIG_DEBUG_ERR,"Unsupported get_func %d\n", func);
			return -RIG_EINVAL;
		}

		return RIG_OK;
}

/*
 * jrc_set_level
 * Assumes rig!=NULL
 * FIXME: cannot support PREAMP and ATT both at same time (make sens though)
 */
int jrc_set_level(RIG *rig, vfo_t vfo, setting_t level, value_t val)
{
		struct jrc_priv_caps *priv = (struct jrc_priv_caps*)rig->caps->priv;
		int cmd_len;
		char cmdbuf[BUFSZ];

		/* Optimize:
		 *   sort the switch cases with the most frequent first
		 */
		switch (level) {
		case RIG_LEVEL_ATT:
		  cmd_len = sprintf(cmdbuf, "A%d" EOM, val.i?1:0);

		  return jrc_transaction (rig, cmdbuf, cmd_len, NULL, NULL);

		case RIG_LEVEL_RF:
		  cmd_len = sprintf(cmdbuf, "HH%03d" EOM, (int)(val.f*255.0));

		  return jrc_transaction (rig, cmdbuf, cmd_len, NULL, NULL);

		case RIG_LEVEL_AF:
		  cmd_len = sprintf(cmdbuf, "JJ%03d" EOM, (int)(val.f*255.0));

		  return jrc_transaction (rig, cmdbuf, cmd_len, NULL, NULL);

		case RIG_LEVEL_SQL:
		  cmd_len = sprintf(cmdbuf, "JJ%03d" EOM, (int)(val.f*255.0));

		  return jrc_transaction (rig, cmdbuf, cmd_len, NULL, NULL);

		case RIG_LEVEL_NOTCHF:
		  cmd_len = sprintf(cmdbuf, "FF%+04d" EOM, val.i);

		  return jrc_transaction (rig, cmdbuf, cmd_len, NULL, NULL);

		case RIG_LEVEL_AGC:
		  if (val.i < 10)
		  	cmd_len = sprintf(cmdbuf, "G%d" EOM, 
				  val.i == RIG_AGC_SLOW ? 0 : 
				  val.i == RIG_AGC_FAST ? 1 : 2);
		  else
		  	cmd_len = sprintf(cmdbuf, "G3%03d" EOM, val.i/20);

		  return jrc_transaction (rig, cmdbuf, cmd_len, NULL, NULL);

		case RIG_LEVEL_CWPITCH:
		  cmd_len = sprintf(cmdbuf, "%s%+05d" EOM, priv->cw_pitch, val.i);

		  return jrc_transaction (rig, cmdbuf, cmd_len, NULL, NULL);

		case RIG_LEVEL_IF:
		  if (priv->pbs_len == 3)
			  val.i /= 10;

		  cmd_len = sprintf(cmdbuf, "P%+0*d" EOM, priv->pbs_len+1, val.i);

		  return jrc_transaction (rig, cmdbuf, cmd_len, NULL, NULL);

		default:
		  rig_debug(RIG_DEBUG_ERR,"Unsupported set_level %d\n", level);
		  return -RIG_EINVAL;
		}

		return RIG_OK;
}


/*
 * jrc_get_level
 * Assumes rig!=NULL, val!=NULL
 */
int jrc_get_level(RIG *rig, vfo_t vfo, setting_t level, value_t *val)
{
		struct jrc_priv_caps *priv = (struct jrc_priv_caps*)rig->caps->priv;
		int retval, lvl_len, lvl;
		char lvlbuf[BUFSZ];
		char cwbuf[BUFSZ];
		int cw_len;

		/* Optimize:
		 *   sort the switch cases with the most frequent first
		 */
		switch (level) {
		case RIG_LEVEL_RAWSTR:
		  /* read A/D converted value */
		  retval = jrc_transaction (rig, "M" EOM, 2, lvlbuf, &lvl_len);
		  if (retval != RIG_OK)
		    return retval;
		  
		  if (lvl_len != 5) {
		    rig_debug(RIG_DEBUG_ERR,"jrc_get_level: wrong answer"
			      "len=%d\n", lvl_len);
		    return -RIG_ERJCTED;
		  }
		  
		  lvlbuf[4] = '\0';
		  val->i = atoi(lvlbuf+1);
		  break;
		  
		case RIG_LEVEL_SQLSTAT:
		  return -RIG_ENIMPL;	/* get_dcd ? */

		case RIG_LEVEL_ATT:
		  //retval = jrc_transaction (rig, "A" EOM, 2, lvlbuf, &lvl_len);
		  retval = jrc_transaction (rig, "I" EOM, 2, lvlbuf, &lvl_len);
		  if (retval != RIG_OK)
		    return retval;

		  //if (lvl_len != 3) {
		if (lvlbuf[0] != 'I' || lvl_len != priv->info_len) {
		    rig_debug(RIG_DEBUG_ERR,"jrc_get_level: wrong answer"
			      "len=%d\n", lvl_len);
		    return -RIG_ERJCTED;
		  }

		  val->i = lvlbuf[1] == '1' ? 20 : 0;
		  break;

		case RIG_LEVEL_RF:
		  retval = jrc_transaction (rig, "HH" EOM, 3, lvlbuf, &lvl_len);
		  if (retval != RIG_OK)
		    return retval;
		  
		  if (lvl_len != 6) {
		    rig_debug(RIG_DEBUG_ERR,"jrc_get_level: wrong answer"
			      "len=%d\n", lvl_len);
		    return -RIG_ERJCTED;
		  }
		  /*
		   * 000..255
		   */
		  sscanf(lvlbuf+2, "%u", &lvl);
		  val->f = (float)lvl/255.0;
		  break;

		case RIG_LEVEL_AF:
		  retval = jrc_transaction (rig, "JJ" EOM, 3, lvlbuf, &lvl_len);
		  if (retval != RIG_OK)
		    return retval;
		  
		  if (lvl_len != 6) {
		    rig_debug(RIG_DEBUG_ERR,"jrc_get_level: wrong answer"
			      "len=%d\n", lvl_len);
		    return -RIG_ERJCTED;
		  }
		  /*
		   * 000..255
		   */
		  sscanf(lvlbuf+2, "%u", &lvl);
		  val->f = (float)lvl/255.0;
		  break;
		  
		case RIG_LEVEL_SQL:
		  retval = jrc_transaction (rig, "LL" EOM, 3, lvlbuf, &lvl_len);
		  if (retval != RIG_OK)
		    return retval;
		  
		  if (lvl_len != 6) {
		    rig_debug(RIG_DEBUG_ERR,"jrc_get_level: wrong answer"
			      "len=%d\n", lvl_len);
		    return -RIG_ERJCTED;
		  }
		  /*
		   * 000..255
		   */
		  sscanf(lvlbuf+2, "%u", &lvl);
		  val->f = (float)lvl/255.0;
		  break;
		  
		case RIG_LEVEL_NOTCHF:
		  retval = jrc_transaction (rig, "GG" EOM, 3, lvlbuf, &lvl_len);
		  if (retval != RIG_OK)
		    return retval;
		  
		  if (lvl_len != 8) {
		    rig_debug(RIG_DEBUG_ERR,"jrc_get_level: wrong answer"
			      "len=%d\n", lvl_len);
		    return -RIG_ERJCTED;
		  }
		  /*
		   * 000..255
		   */
		  sscanf(lvlbuf+2, "%d", &lvl);
		  val->f = (float)lvl/255.0;
		  break;

		case RIG_LEVEL_CWPITCH:
		  cw_len = sprintf(cwbuf, "%s" EOM, priv->cw_pitch);

		  retval = jrc_transaction (rig, cwbuf, cw_len, lvlbuf, &lvl_len);
		  if (retval != RIG_OK)
		    return retval;
		  
		  if (lvl_len != cw_len+5) {
		    rig_debug(RIG_DEBUG_ERR,"jrc_get_level: wrong answer"
			      "len=%d\n", lvl_len);
		    return -RIG_ERJCTED;
		  }

		  sscanf(lvlbuf+(cw_len-1), "%05d", &lvl);
		  val->i = lvl;
		  break;

		case RIG_LEVEL_IF:
		  retval = jrc_transaction (rig, "P" EOM, 2, lvlbuf, &lvl_len);
		  if (retval != RIG_OK)
		    return retval;
		  
		if (lvlbuf[0] != 'P' || lvl_len != priv->pbs_info_len) {
		    rig_debug(RIG_DEBUG_ERR,"jrc_get_level: wrong answer"
			      "len=%d\n", lvl_len);
		    return -RIG_ERJCTED;
		  }

		  sscanf(lvlbuf+1, "%d", &lvl);
		  if (priv->pbs_len == 3)
		    lvl *= 10;

		  val->i = lvl;
		  break;

		default:
		  rig_debug(RIG_DEBUG_ERR,"Unsupported get_level %d\n", level);
		  return -RIG_EINVAL;
		}

		return RIG_OK;
}

/*
 * jrc_set_parm
 * Assumes rig!=NULL
 * FIXME: cannot support PREAMP and ATT both at same time (make sens though)
 */
int jrc_set_parm(RIG *rig, setting_t parm, value_t val)
{
		struct jrc_priv_caps *priv = (struct jrc_priv_caps*)rig->caps->priv;
		int cmd_len;
		char cmdbuf[BUFSZ];
		int minutes;

		/* Optimize:
		 *   sort the switch cases with the most frequent first
		 */
		switch (parm) {
		case RIG_PARM_BACKLIGHT:
			cmd_len = sprintf(cmdbuf, "AA%d" EOM, val.f>0.5?0:1);

			return jrc_transaction (rig, cmdbuf, cmd_len, NULL, NULL);

		case RIG_PARM_BEEP:

		  //cmd_len = sprintf(cmdbuf, "U%03d" EOM, val.i?101:100);
		  cmd_len = sprintf(cmdbuf, "U%0*d" EOM, priv->beep_len, priv->beep + val.i?1:0);
		  
		  return jrc_transaction (rig, cmdbuf, cmd_len, NULL, NULL);

		case RIG_PARM_TIME:
		  minutes = val.i/60;
		  cmd_len = sprintf(cmdbuf, "R1%02d%02d" EOM, 
				    minutes/60, minutes%60);
		  
		  return jrc_transaction (rig, cmdbuf, cmd_len, NULL, NULL);
		  
		default:
		  rig_debug(RIG_DEBUG_ERR,"Unsupported set_parm %d\n", parm);
		  return -RIG_EINVAL;
		}
		
		return RIG_OK;
}

/*
 * jrc_get_parm
 * Assumes rig!=NULL, val!=NULL
 */
int jrc_get_parm(RIG *rig, setting_t parm, value_t *val)
{
                int retval, lvl_len, i;
		char lvlbuf[BUFSZ];

		/* Optimize:
		 *   sort the switch cases with the most frequent first
		 */
		switch (parm) {
		case RIG_PARM_TIME:
		  retval = jrc_transaction (rig, "R0" EOM, 3, lvlbuf, &lvl_len);
		  if (retval != RIG_OK)
		    return retval;
		  
		  /* "Rhhmmss"CR */
		  if (lvl_len != 8) {
		    rig_debug(RIG_DEBUG_ERR,"jrc_get_parm: wrong answer"
			      "len=%d\n", lvl_len);
		    return -RIG_ERJCTED;
		  }
		  
		  /* convert ASCII to numeric 0..9 */
		  for (i=1; i<7; i++) {
		    lvlbuf[i] -= '0';
		  }
		  val->i = ((10*lvlbuf[1] + lvlbuf[2])*60 +	/* hours */
			     10*lvlbuf[3] + lvlbuf[4])*60 +	/* minutes */
		             10*lvlbuf[5] + lvlbuf[6];	/* secondes */
		  break;
		  
		case RIG_PARM_BEEP:
		  retval = jrc_transaction (rig, "U9" EOM, 3, lvlbuf, &lvl_len);
		  if (retval != RIG_OK)
		    return retval;
		  
		  if (lvl_len != 4) {
		    rig_debug(RIG_DEBUG_ERR,"jrc_get_parm: wrong answer"
			      "len=%d\n", lvl_len);
		    return -RIG_ERJCTED;
		  }

		  val->i = lvlbuf[2] == 0 ? 0 : 1;
		  break;
		  
		default:
		  rig_debug(RIG_DEBUG_ERR,"Unsupported get_parm %d\n", parm);
		  return -RIG_EINVAL;
		}

		return RIG_OK;
}

/*
 * jrc_get_dcd
 * Assumes rig!=NULL, dcd!=NULL
 */
int jrc_get_dcd(RIG *rig, vfo_t vfo, dcd_t *dcd)
{
		char dcdbuf[BUFSZ];
		int dcd_len, retval;

		retval = jrc_transaction (rig, "Q" EOM, 2, dcdbuf, &dcd_len);
		if (retval != RIG_OK)
			return retval;

		if (dcd_len != 3) {
			rig_debug(RIG_DEBUG_ERR,"jrc_get_dcd: wrong answer %s, "
							"len=%d\n", dcdbuf, dcd_len);
			return -RIG_ERJCTED;
		}
		*dcd = dcdbuf[1] == '0' ? RIG_DCD_ON : RIG_DCD_OFF;

		return RIG_OK;
}

/*
 * jrc_set_trn
 * Assumes rig!=NULL
 */
int jrc_set_trn(RIG *rig, int trn)
{
		unsigned char trnbuf[BUFSZ];
		int trn_len;

		trn_len = sprintf(trnbuf, "I%d" EOM, trn==RIG_TRN_RIG?1:0);

		return jrc_transaction (rig, trnbuf, trn_len, NULL, NULL);
}

/*
 * jrc_set_powerstat
 * Assumes rig!=NULL
 */
int jrc_set_powerstat(RIG *rig, powerstat_t status)
{
		unsigned char pwrbuf[BUFSZ];
		int pwr_len;

		pwr_len = sprintf(pwrbuf, "T%d" EOM, status==RIG_POWER_ON?1:0);

		return jrc_transaction (rig, pwrbuf, pwr_len, NULL, NULL);
}

/*
 * jrc_reset
 * Assumes rig!=NULL
 */
int jrc_reset(RIG *rig, reset_t reset)
{
		unsigned char rstbuf[BUFSZ];
		int rst_len;
		char rst;

		switch(reset) {
			case RIG_RESET_MCALL: rst='1'; break;	/* mem clear */
			case RIG_RESET_VFO: rst='2'; break;		/* user setup default */
			case RIG_RESET_MASTER: rst='3'; break;	/* 1 + 2 */
			default: 
				rig_debug(RIG_DEBUG_ERR,"jrc_reset: unsupported reset %d\n",
								reset);
				return -RIG_EINVAL;
		}
		rst_len = sprintf(rstbuf, "Z%c" EOM, rst);

		return jrc_transaction (rig, rstbuf, rst_len, NULL, NULL);
}

/*
 * jrc_set_mem
 * Assumes rig!=NULL
 */
int jrc_set_mem(RIG *rig, vfo_t vfo, int ch)
{
		char cmdbuf[BUFSZ];
		char membuf[BUFSZ];
		int cmd_len, mem_len;

		if (ch < 0 || ch > rig->caps->chan_list[0].end)
		  return -RIG_EINVAL;

		cmd_len = sprintf(cmdbuf, "C%03u" EOM, ch);

		/* don't care about the Automatic reponse from receiver */

		return jrc_transaction (rig, cmdbuf, cmd_len, membuf, &mem_len);
}

/*
 * jrc_get_mem
 * Assumes rig!=NULL
 */
int jrc_get_mem(RIG *rig, vfo_t vfo, int *ch)
{
		struct jrc_priv_caps *priv = (struct jrc_priv_caps*)rig->caps->priv;
		int mem_len, retval;
		char membuf[BUFSZ];
		int chan;

		retval = jrc_transaction (rig, "L" EOM, 2, membuf, &mem_len);
		if (retval != RIG_OK)
		  return retval;

		if (mem_len != priv->mem_len) {
		  rig_debug(RIG_DEBUG_ERR,"jrc_get_mem: wrong answer %s, "
			    "len=%d\n", membuf, mem_len);
		  return -RIG_ERJCTED;
		}
		membuf[4] = '\0';

		/*extract current channel*/
		sscanf(membuf+1, "%d", &chan);
		*ch = chan;

		return RIG_OK;
}

/*
 * jrc_vfo_op
 * Assumes rig!=NULL
 */
int jrc_vfo_op(RIG *rig, vfo_t vfo, vfo_op_t op)
{
		const char *cmd;

		switch(op) {
			case RIG_OP_FROM_VFO: cmd="E1" EOM; break;
			default: 
				rig_debug(RIG_DEBUG_ERR,"jrc_vfo_op: unsupported op %#x\n",
								op);
				return -RIG_EINVAL;
		}

		return jrc_transaction (rig, cmd, 3, NULL, NULL);
}


/*
 * jrc_scan, scan operation
 * Assumes rig!=NULL
 *
 * Not really a scan operation so speaking. 
 * You just make the rig increment frequency of decrement continuously,
 * depending on the sign of ch.
 * However, using DCD sensing, followed by a stop, you get it.
 */
int jrc_scan(RIG *rig, vfo_t vfo, scan_t scan, int ch)
{
		const char *scan_cmd = "";

		switch(scan) {
			case RIG_SCAN_STOP:
					scan_cmd = "Y0" EOM;
					break;
			case RIG_SCAN_SLCT:
					scan_cmd = ch > 0 ? "Y+" EOM : "Y-" EOM;
					break;
			default:
				rig_debug(RIG_DEBUG_ERR,"Unsupported scan %#x", scan);
				return -RIG_EINVAL;
		}

		return jrc_transaction (rig, scan_cmd, 3, NULL, NULL);
}

static int jrc2rig_mode(RIG *rig, char jmode, char jwidth, 
				rmode_t *mode, pbwidth_t *width)
{
		switch (jmode) {
		case MD_CW:	*mode = RIG_MODE_CW; break;
		case MD_USB:	*mode = RIG_MODE_USB; break;
		case MD_LSB:	*mode = RIG_MODE_LSB; break;
		case MD_FM:	*mode = RIG_MODE_FM; break;
		case MD_AM:	*mode = RIG_MODE_AM; break;
		case MD_RTTY:	*mode = RIG_MODE_RTTY; break;
		case MD_WFM:	*mode = RIG_MODE_WFM; break;
		default:
		  rig_debug(RIG_DEBUG_ERR,
			    "jrc_set_mode: unsupported mode %c\n",
			    jmode);
		  *mode = RIG_MODE_NONE;
		  return -RIG_EINVAL;
		}

		/*
		 * determine passband
		 */
		switch (jwidth) {
		case '0':
		  *width = rig_passband_wide(rig, *mode);
		  break;
		case '1':
		  *width = rig_passband_normal(rig, *mode);
		  break;
		case '2':
		  *width = rig_passband_narrow(rig, *mode);
		  break;
		default:
		  rig_debug(RIG_DEBUG_ERR,
			    "jrc_set_mode: unsupported width %c\n",
			    jwidth);
		  *width = RIG_PASSBAND_NORMAL;
		  return -RIG_EINVAL;
		}

		return RIG_OK;
}

/*
 * jrc_decode is called by sa_sigio, when some asynchronous
 * data has been received from the rig
 */
int jrc_decode_event(RIG *rig)
{
		struct jrc_priv_caps *priv = (struct jrc_priv_caps*)rig->caps->priv;
		struct rig_state *rs;
		freq_t freq;
		rmode_t mode;
		pbwidth_t width; 
		int count;
		char buf[BUFSZ];

		rig_debug(RIG_DEBUG_VERBOSE, "jrc: jrc_decode called\n");

		rs = &rig->state;

		/* "Iabdfg"CR */
		//#define SETUP_STATUS_LEN 17

		//count = read_string(&rs->rigport, buf, SETUP_STATUS_LEN, "", 0);
		count = read_string(&rs->rigport, buf, priv->info_len, "", 0);
		if (count < 0) {
		  return count;
		}
		buf[31] = '\0';	/* stop run away.. */

		if (buf[0] != 'I') {
		  rig_debug(RIG_DEBUG_WARN, "jrc: unexpected data: %s\n",
			    buf);
		  return -RIG_EPROTO;
		}

		/*
		 * TODO: Attenuator and AGC change notification.
		 */

		if (rig->callbacks.freq_event) {
		  long long f;
		  
		  //buf[14] = '\0';	/* side-effect: destroy AGC first digit! */
		  buf[4+priv->max_freq_len] = '\0';	/* side-effect: destroy AGC first digit! */
		  sscanf(buf+4, "%lld", &f);
		  freq = f;
		  return rig->callbacks.freq_event(rig, RIG_VFO_CURR, freq,
						   rig->callbacks.freq_arg);
		}
		
		if (rig->callbacks.mode_event) {
		  jrc2rig_mode(rig, buf[3], buf[2], &mode, &width);
		  return rig->callbacks.mode_event(rig, RIG_VFO_CURR, mode, width,
						   rig->callbacks.freq_arg);
		}
		
		return RIG_OK;
}


/*
 * initrigs_jrc is called by rig_backend_load
 */
DECLARE_INITRIG_BACKEND(jrc)
{
		rig_debug(RIG_DEBUG_VERBOSE, "jrc: _init called\n");

		rig_register(&nrd535_caps);
		rig_register(&nrd545_caps);

		return RIG_OK;
}



