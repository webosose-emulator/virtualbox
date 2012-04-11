/** @file
 *
 * VBox frontends: Qt4 GUI ("VirtualBox"):
 * UIWizardImportApp class declaration
 */

/*
 * Copyright (C) 2009-2012 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef __UIWizardImportApp_h__
#define __UIWizardImportApp_h__

/* Local includes */
#include "UIWizard.h"

/* Import Appliance wizard: */
class UIWizardImportApp : public UIWizard
{
    Q_OBJECT;

public:

    /* Page IDs: */
    enum
    {
        Page1,
        Page2
    };

    /* Constructor: */
    UIWizardImportApp(const QString &strFileName = QString(), QWidget *pParent = 0);

    /* Is appliance valid? */
    bool isValid() const;

protected:

    /* Import stuff: */
    bool importAppliance();

    /* Who will be able to import appliance: */
    friend class UIWizardImportAppPageBasic2;

private slots:

    /* Page change handler: */
    void sltCurrentIdChanged(int iId);

private:

    /* Translation stuff: */
    void retranslateUi();
};

#endif /* __UIWizardImportApp_h__ */

