/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */
/*
    QM DSP Library

    Centre for Digital Music, Queen Mary, University of London.
    This file 2005-2006 Christian Landone.

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.
*/

#ifndef DECIMATOR_H
#define DECIMATOR_H

class Decimator  
{
public:
    void process( const double* src, double* dst );
    void process( const float* src, float* dst );

    /**
     * Construct a Decimator to operate on input blocks of length
     * inLength, with decimation factor decFactor.  inLength should be
     * a multiple of decFactor.  Output blocks will be of length
     * inLength / decFactor.
     *
     * decFactor must be a power of two.  The highest supported factor
     * is obtained through getHighestSupportedFactor(); for higher
     * factors, you will need to chain more than one decimator.
     */
    Decimator( unsigned int inLength, unsigned int decFactor );
    virtual ~Decimator();

    int getFactor() const { return m_decFactor; }
    static int getHighestSupportedFactor() { return 8; }

private:
    void resetFilter();
    void deInitialise();
    void initialise( unsigned int inLength, unsigned int decFactor );
    void doAntiAlias( const double* src, double* dst, unsigned int length );
    void doAntiAlias( const float* src, double* dst, unsigned int length );

    unsigned int m_inputLength;
    unsigned int m_outputLength;
    unsigned int m_decFactor;

    double Input;
    double Output ;

    double o1,o2,o3,o4,o5,o6,o7;

    double a[ 9 ];
    double b[ 9 ];
	
    double* decBuffer;
};

#endif // 
