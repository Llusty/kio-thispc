// Discovery uses an isolated XDG configuration and disposable files only.
static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir files;
    verify(files.isValid(), "temporary template root");
    const QString templates = files.filePath(QStringLiteral("Szablony użytkownika"));
    const QString configPath = QString::fromLocal8Bit(qgetenv("XDG_CONFIG_HOME"));
    verify(!configPath.isEmpty() && QDir().mkpath(configPath), "isolated XDG config");
    auto configure = [&](const QString &path) {
        QFile config(configPath + "/user-dirs.dirs");
        verify(config.open(QIODevice::WriteOnly | QIODevice::Truncate), "open isolated user dirs");
        const QByteArray data = "XDG_TEMPLATES_DIR=\"" + path.toUtf8() + "\"\n";
        verify(config.write(data) == data.size(), "configure native templates location");
        config.close();
        verify(QStandardPaths::writableLocation(QStandardPaths::TemplatesLocation) == path,
               "Qt resolves the configured XDG templates directory");
    };
    configure(templates);
    TemplateMenu menu;
    auto refresh = [&] {
        menu.close();
        menu.popup(QPoint(10, 10));
        verify(QTest::qWaitFor([&] { return !menu.m_job; }, 5000), "asynchronous discovery completes");
    };
    auto empty = [&] {
        verify(menu.actions().size() == 1 && !menu.actions().first()->isEnabled()
                   && menu.actions().first()->text() == "No templates found",
               "empty or missing directory has a disabled placeholder");
    };
    refresh();
    empty();
    verify(!QFileInfo::exists(templates), "discovery does not create the missing directory");
    verify(QDir().mkpath(templates), "create template directory");
    refresh();
    empty();

    auto write = [&](const QString &name) {
        QFile file(templates + "/" + name);
        verify(file.open(QIODevice::WriteOnly) && file.write("template data") == 13,
               "create disposable template");
    };
    write("zeta.txt");
    write("A&B.md");
    write(QStringLiteral("Żółw.odt"));
    write(".hidden");
    write("recipe.desktop");
    verify(QDir().mkpath(templates + "/folder"), "directory fixture");
    write("folder/nested.txt");
    verify(QFile::link(templates + "/zeta.txt", templates + "/link.txt"), "symlink fixture");
    verify(::mkfifo(QFile::encodeName(templates + "/pipe").constData(), 0600) == 0,
           "special file fixture");
    refresh();
    QStringList names;
    for (QAction *action : menu.actions()) names << action->text();
    verify(names == QStringList{"A&&B.md", "recipe.desktop", "zeta.txt", QStringLiteral("Żółw.odt")},
           "menu sorts regular files, escapes mnemonics, excludes hidden files, folders, links and special files");
    verify(menu.toolTip() == templates, "menu exposes the native template directory");
    QUrl selected;
    QObject::connect(&menu, &TemplateMenu::templateSelected, &menu, [&](const QUrl &url) { selected = url; });
    menu.actions().first()->trigger();
    verify(selected == QUrl::fromLocalFile(templates + "/A&B.md"), "action emits exact source URL");
    verify(menu.actions().at(1)->data().toUrl() == QUrl::fromLocalFile(templates + "/recipe.desktop"),
           "desktop files remain literal source documents");

    verify(QFile::remove(templates + "/zeta.txt"), "remove disposable template");
    write("Beta.txt");
    refresh();
    names.clear();
    for (QAction *action : menu.actions()) names << action->text();
    verify(names.contains("Beta.txt") && !names.contains("zeta.txt") && names.size() == 4,
           "reopening reflects additions and removals without duplicates");
    menu.aboutToShow();
    menu.aboutToShow();
    verify(QTest::qWaitFor([&] { return !menu.m_job; }, 5000) && menu.actions().size() == 4,
           "overlapping refreshes discard the previous listing");

    configure(templates + "/A&B.md");
    refresh();
    verify(menu.actions().size() == 1 && !menu.actions().first()->isEnabled()
               && menu.actions().first()->text() == "Could not read templates",
           "listing failure removes stale actions and shows a disabled error");
    configure(QDir::homePath());
    refresh();
    empty();
    verify(!menu.m_job, "disabled XDG templates never scan home");

    configure(templates);
    auto *pending = new TemplateMenu;
    pending->aboutToShow();
    QPointer<KIO::ListJob> job = pending->m_job;
    verify(job != nullptr, "discovery starts asynchronously");
    delete pending;
    verify(QTest::qWaitFor([&] { return !job; }, 5000), "destroying menu cancels its pending listing");
    qInfo("PASS: %d template menu assertions; native XDG discovery, filtering, refresh and lifecycle", checks);
}
