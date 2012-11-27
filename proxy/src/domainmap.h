#ifndef DOMAINMAP_H
#define DOMAINMAP_H

#include <QPair>
#include <QString>

// this class offers fast access to the domains file. the table is maintained
//   by a background thread so that file access doesn't cause blocking.

class DomainMap
{
public:
	typedef QPair<QString, int> Target;

	DomainMap(const QString &fileName);
	~DomainMap();

	QList<Target> entry(const QString &domain) const;

private:
	class Private;
	Private *d;

	class Thread;
	class Worker;
};

#endif
