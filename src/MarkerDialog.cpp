#include "MarkerDialog.h"
#include <QFormLayout>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QDialogButtonBox>

MarkerDialog::MarkerDialog(const Marker &m, int sampleRate, QWidget *parent)
    : QDialog(parent), m_(m) {
    setWindowTitle(tr("Marker"));
    auto *form = new QFormLayout;
    auto *nameEdit = new QLineEdit(m.name);
    nameEdit->setPlaceholderText(tr("(unnamed)"));
    form->addRow(tr("Name:"), nameEdit);

    double sec = sampleRate ? double(m.timeSamples) / sampleRate : m.timeSec;
    auto *timeLbl = new QLabel(QStringLiteral("%1  (%2 samples)")
        .arg(QString::number(sec, 'f', 3)).arg(m.timeSamples));
    form->addRow(tr("Time:"), timeLbl);

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto *delBtn = new QPushButton(tr("Delete marker"));
    btns->addButton(delBtn, QDialogButtonBox::DestructiveRole);

    connect(btns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(delBtn, &QPushButton::clicked, this, [this] { del_ = true; accept(); });

    auto *lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addWidget(btns);

    connect(nameEdit, &QLineEdit::textChanged, this, [this](const QString &t) { m_.name = t; });
}

Marker MarkerDialog::marker() const { return m_; }
