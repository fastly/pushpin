#ifndef INSPECTDATA_H
#define INSPECTDATA_H

class InspectData
{
public:
	bool doProxy;
	QByteArray sharingKey;

	InspectData() :
		doProxy(false)
	{
	}
};

#endif
